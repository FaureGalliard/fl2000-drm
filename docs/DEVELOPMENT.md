# Development notes — hardware map and debugging history

Companion to `CLAUDE.md`. This file records *why* the driver looks the way it
does, so the same dead ends are not explored twice.

## Hardware map

```
USB 3.0 ──▶ FL2000DX ──┬── I²C master (regmap over USB control transfers)
                       │        │
                       │        ├── IT66121 HDMI bridge  @ 0x4C
                       │        └── monitor DDC/EDID     @ 0x50   ← EDID comes from here
                       │
                       ├── bulk EP1 OUT ── pixel stream ──▶ IT66121 ──▶ HDMI
                       │
                       ├── interrupt EP3 ── HPD / status events
                       │
                       └── mass storage function (virtual CD with Windows drivers)
```

The dongle additionally contains an unrelated **USB2 hub + C-Media audio chip**
that enumerate as their own USB devices.

Two things about this topology are counter-intuitive and cost a lot of time:

1. **The monitor's DDC lines hang off the FL2000 I²C bus (address 0x50), not off
   the IT66121's DDC master.** The IT66121 EDID FIFO path — which the in-tree
   `ite-it66121` driver uses, and which the original klogg driver implemented —
   never completes on this board. The vendor (Fresco) driver reads EDID at 0x50
   with the FL2000's own dword I²C engine, and that is what works here.
   `it66121_get_edid_block_direct()` implements it.

2. **The virtual CD-ROM function actively breaks the display.** `udisks` probes
   it, the storage function stops responding once the display driver resets the
   chip, and 30 s later the SCSI error handler issues `usb_reset_device()` on the
   *whole* device, tearing the display down. The `usb-storage` quirk
   (`1d5c:2000:i`) is not optional — see the README troubleshooting section, and
   note that `usb_storage` loads from the initramfs, so `mkinitcpio -P` is
   required after creating the modprobe.d file.

## Decoding the driver's DDC messages

`it66121_wait_ddc_ready()` logs the raw `IT66121_DDC_STATUS` (0x16) register:

| Bit | Mask | Meaning |
|-----|------|---------|
| 7 | 0x80 | TX done — the only reliable success signal |
| 6 | 0x40 | Engine active (transfer in progress) |
| 5 | 0x20 | NOACK — the target did not acknowledge |
| 4 | 0x10 | Waiting for bus |
| 3 | 0x08 | Arbitration lost |

Values seen in the field:

- `0x1A` (wait bus + arbitration lose, idle) — the DDC master cannot get the
  bus. Originally caused by the HDCP engine holding it after chip reset; fixed
  by restoring the ITE release sequence in `it66121_abort_ddc_ops()`.
- `0x4A` (active + arbitration lose) — the engine is running but keeps losing
  arbitration. On this board the FL2000 I²C master and the IT66121 DDC master
  share the physical bus, so **arbitration loss is a normal transient** caused
  partly by our own status polling. Do not treat error bits as fatal; wait for
  `TX_DONE` or the timeout, and poll sparsely.

`DDC not done (-110)` on the FIFO path followed by `EDID read ok` is the
**expected** sequence on this hardware: the FIFO attempt fails, the direct path
succeeds.

## Bug history (symptom → root cause)

Each of these was a real failure observed on hardware, in the order they were
found. All are fixed.

| Symptom | Root cause |
|---|---|
| Hard kernel hang on disconnect | `it66121_destroy()` → `component_del()` called from the master unbind callback, which already holds the global component mutex → self-deadlock |
| Display torn down ~30 s after every plug | SCSI timeout on the virtual CD → `usb_reset_device()` on the whole dongle |
| Driver never re-binds after a USB reset | Probe bailed out with `-ENODEV` because the bridge had been destroyed on disconnect while `devs` survived |
| `modprobe` appears to freeze the terminal | Probe runs synchronously in modprobe context while the device is stuck in the reset cycle; not a driver hang. Load the module with the adapter unplugged, then plug it (probe runs in a udev worker) |
| Only fallback modes (1024x768…), EDID empty | `drm_get_edid()` on the FL2000 I²C bus hit the adapter's 1-byte-read quirks (`msg too long (addr 0x0050)`). The 6.x modernization had replaced the working DDC-FIFO reader with it |
| EDID reads failing silently | No error logging anywhere in the DDC path; added logging first, then fixed the actual failures |
| `DDC not ready`, status `0x1A` | HDCP engine holding the DDC bus after chip reset; the original driver had deleted the ITE release sequence as "HDCP is not supported" |
| `DDC ... 0x4A`, EDID still failing | Fail-fast on error bits aborted healthy transfers on a shared, contended bus |
| Kernel Oops in `component_bind_all()` on rebind | Destroying/re-creating the component per bind cycle leaves dangling pointers in the aggregate match data |
| `failed to create dumb buffer: Cannot allocate memory` | GEM DMA (CMA) helpers require ~8.3 MB physically contiguous for 1080p; impossible on x86 without CMA. Switched to GEM SHMEM |
| Desktop-wide lag, amdgpu `Fence fallback timer expired` | Multi-MB `memcpy`/pixel conversion executed while holding `list_lock`, which disables interrupts. The resend copy runs continuously even on a static desktop |

## Findings from the upstream clean-up (October 2026)

Found while reading the code line by line for kernel style. None had shown up
as a field failure, which is exactly why they are recorded here.

| Finding | What was done |
|---|---|
| `fl2000_mode_calc()` loop starts with `s = 0`, so the htotal adjustment `d` is always 0 and the same PLL search runs 21 times. Modes the PLL cannot hit within 500 ppm are rejected — including 1920x1080@60.00 (148.5 MHz); the desktop runs at 59.94 Hz because of it | **Not applied** (changes the mode sent to the monitor). Fix + simulation results in `docs/pending/0001-*.patch` |
| IT66121 `INT_STATUS_1` was decoded through a bitfield union with 24 padding bits first, so all 8 flags landed at bits 24-31 of an 8-bit register value and always read 0. The DDC-recovery and HPD branches of `it66121_intr_work()` never ran ("Monitor HPD" never appears in any log) | Dead branches removed (no functional change); correct bit defines kept. Enabling them needs HPD to be cleared via `INT_CLR` or it would fire every 100 ms |
| Interrupt URB resubmitted with `GFP_KERNEL` + `cond_resched()`, and `usb_clear_halt()` on stall, both inside URB completion (atomic context) | Fixed: `GFP_ATOMIC`, stall treated as error |
| `fl2000_i2c_init()` failure path called `devres_free()` on an already-added devres → `BUG_ON` | Fixed: `devm_kzalloc()` + `devm_i2c_add_adapter()` |
| `fl2000_drm_bind()` did not check `fl2000_stream_create()`/`fl2000_intr_create()` for `ERR_PTR` and had no unwinding | Fixed |
| Stream disable could hang in `drain_workqueue()` if no URB was in flight to `up()` the semaphore the work slept on | Semaphore replaced by an atomic credit counter + wait queue; disable wakes the work |
| FL2000 register bitfield unions and `fl2000_add_bitmask()` | Replaced by `GENMASK()`; proven identical with a mocked regmap that logs all 691 register writes of both versions |

The FL2000 register helpers can be unit-tested this way: compile
`fl2000_registers.c` in userspace against a stub `fl2000.h` that provides
logging `regmap_write()`/`regmap_write_bits()`/`regmap_read()` and the
`BIT`/`GENMASK`/`FIELD_PREP` macros, then diff the logs of two versions.

## Rejected theories

- *"Kernel 7.x broke the DRM API broadly."* It did not. Almost everything the
  driver uses still exists in 7.1 (`drm_simple_display_pipe`, legacy bridge
  callbacks, `drm_get_edid`, `create_workqueue`, `REGCACHE_RBTREE`). The only
  mandatory change was bridge refcounting. (Several of these are deprecated
  and were replaced or are listed as upstream review items anyway.)
- *"The hardware or the monitor is faulty."* The same adapter and monitor work
  under Windows; every failure was driver-side.
- *"The EDID FIFO path just needs better timing."* It never completes on this
  board regardless of timing; the wiring is different (see Hardware map).

## Verifying kernel APIs

`git.kernel.org` blocks automated fetches (Anubis). Use the GitHub mirror:

```
https://raw.githubusercontent.com/torvalds/linux/v7.1/<path>
```

Download headers locally and grep them rather than trusting search results or
recollection — several of the fixes above came from reading
`drivers/base/component.c` and `drivers/gpu/drm/drm_bridge.c` directly.
