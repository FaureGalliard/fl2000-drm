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
| Machine hangs when the adapter is unplugged while streaming | A data URB whose submission failed (`-ENODEV`) was freed while still on the stream's anchor; `usb_kill_anchored_urbs()` then spun forever on the dangling entry. Fixed with `usb_unanchor_urb()` |
| After an unplug the dongle never enumerates again (`usb_hub_wq` stuck in `i2c_del_adapter()`) | The DRM device was a child of the I2C adapter and so held a reference on it, while `i2c_del_adapter()` waits for all references. The DRM device is now a child of the USB device and `drm_dev_put()` is explicit in `fl2000_drm_if_release()` |
| After a monitor/cable cycle the connector only offers modes up to 1024x768 and an empty EDID | The EDID was cached in the bridge and only handed to the connector the first time; the DRM core drops it when the connector goes through `disconnected`. It is now handed to the connector on every probe and read again only after HPD reported no monitor (reading it on every probe stalled the compositor for ~1 s per probe), starting with the DDC path that worked last time |
| With Mutter 51 the desktop stalls after ~30 min: `amdgpu_dm_plane_helper_prepare_fb ... bind failed`, page flips fail with ENOSPC | Mutter renders ARGB2101010 and tries to scan that buffer out on our plane; AddFB2 failed with EINVAL (`Zero-copy disabled ... format=AR30`) because only XRGB8888 was advertised, so Mutter used its primary-GPU copy path (Mutter issue #5101, a dma-buf leaked per frame) until amdgpu's GTT was full. XRGB2101010/ARGB2101010 are now accepted and converted to 8 bits per channel; needs a long-run check of `mem_info_gtt_used` |
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
| `fl2000_get_bytes_pix()` divides a bandwidth in bytes/s by the pixel clock in **kHz** (callers pass `drm_display_mode.clock`), and the "80 % of USB bandwidth" macros compute `* 100 / 80` (125 %). The result is always ≥ 3, so every mode on every USB speed streams RGB888 and the bandwidth check never rejects anything | **Not changed.** With correct units, USB 3.0 (500 MB/s budget) keeps RGB888 up to 166 MHz (1080p60 unchanged) and drops to RGB565 up to 250 MHz; USB 2.0 (48 MB/s) would reject even 640x480@60. Whether the FL2000 tolerates bulk data arriving slower than the refresh rate (the README claims 1080p30 on USB 2.0) has to be checked on hardware before fixing |
| FL2000 register bitfield unions and `fl2000_add_bitmask()` | Replaced by `GENMASK()`; proven identical with a mocked regmap that logs all 691 register writes of both versions |

The FL2000 register helpers can be unit-tested this way: compile
`fl2000_registers.c` in userspace against a stub `fl2000.h` that provides
logging `regmap_write()`/`regmap_write_bits()`/`regmap_read()` and the
`BIT`/`GENMASK`/`FIELD_PREP` macros, then diff the logs of two versions.

## Unplug and replug (27 October 2026)

State on the `integration` branch after the work of that day:

- **Unplug is clean.** Two kernel-side hangs were fixed (the failed-submit URB
  left on the anchor, and the DRM device parented to the I2C adapter; see the
  bug table). `usb_hub_wq` no longer blocks, the dongle re-enumerates, rebinds,
  reads the EDID and streams again.
- **The driver works after a replug on its own.** From a TTY,
  `scripts/fl2000-field-test.sh` showed the colour bars on the monitor after an
  unplug/replug cycle (`field-test-15_04.log`). The kernel `udev` events are the
  expected ones (`field-udev-replug-14_41.log`: remove of the connector and card,
  then add of the new card and connector, `change HOTPLUG=1`).
- **After `pacman -Syu` on 29 Oct 2026 (kernel 7.1.8 -> 7.2.11, Mutter 50.4 ->
  51.1, gnome-shell 51.1, Mesa 26.2.6)** the driver did not load: out-of-tree
  modules are installed per kernel (`/lib/modules/<version>/extra/`) and the new
  kernel had none. It rebuilt unchanged against 7.2.11 and loaded
  (`sudo install -D -m 644 fl2000.ko /lib/modules/$(uname -r)/extra/fl2000.ko`,
  `depmod -a`). With Mutter 51.1 the first replug of the session brought the
  monitor back (the DRM node changed, card2 -> card3). The next replug got the
  same node again (card2) and failed: `Failed to hotplug secondary gpu
  '/dev/dri/card2': device already present`, then `Failed to use linear monitor
  configuration: No available CRTC for monitor 'GSM 22MP55' not found`, which
  also disabled the monitor on the primary GPU's HDMI port. The dongle's output
  stayed enabled by the kernel's fbdev client (nothing draws on it). So Mutter
  keeps the GPU it was given and cannot add a new one under the same path;
  logging out and in clears it. The kernel side was clean throughout, and the
  EDID is now read once per connection (one `EDID read failed, trying the other
  DDC path` per replug).
- **Open (seen with GNOME/Mutter 50.4): Mutter does not pick the output up** when the dongle
  appears with the session open. The log says `Added device '/dev/dri/cardN'
  (fl2000)` and `Created gbm renderer`, but `GetCurrentState` and
  `GetResources` list only amdgpu. Loading `vkms` once made the monitor appear
  (`modprobe vkms`, query, `modprobe -r vkms`); a second replug with the same
  node name has crashed gnome-shell (SIGSEGV in
  `meta_monitor_config_manager_assign`, after `Failed to hotplug secondary gpu
  ... device already present`). The saved monitor layout gets rearranged as a
  side effect. Reading Mutter's `add_drm_device()` shows no monitor-manager
  reload after a GPU is added; the `gpu-added` handler was not read. Workaround:
  plug the dongle in before logging in, or log out and in.

Tried without effect on the Mutter side (do not retry): probing the connector
before `drm_dev_register()`; synthetic `HOTPLUG=1` uevents on the dongle and on
amdgpu (`echo "change <UUID> HOTPLUG=1" > /sys/class/drm/cardN/uevent`: the
kernel requires the UUID before any `KEY=VALUE`); forcing a real state change
with `echo off` / `echo detect` on the connector `status`; comparing `udev` tags
and `logind` seat membership (identical to amdgpu). Next steps if it is picked up
again: run Mutter with `MUTTER_DEBUG=kms` from login, read its `gpu-added`
handler, and report it upstream with the trace.

klogg/fl2000_drm has no issue about this. Its issue #12 (kernel crash on
removal) was validated with QEMU USB passthrough, without a compositor, and a
commenter there describes the same DRM-parent/`i2c_del_adapter()` deadlock.

## Rejected theories

- *"Kernel 7.x broke the DRM API broadly."* It did not. Almost everything the
  driver uses still exists in 7.1 (`drm_simple_display_pipe`, legacy bridge
  callbacks, `drm_get_edid`, `create_workqueue`, `REGCACHE_RBTREE`). The only
  mandatory change was bridge refcounting. (Several of these are deprecated
  and were replaced or are listed as upstream review items anyway.)
- *"The hardware or the monitor is faulty."* The same adapter and monitor work
  under Windows; every failure was driver-side.
- *"After a replug the driver, not the compositor, fails to bring the output up."*
  It does not: the TTY test shows the picture. What is missing is Mutter
  adding the hot-plugged GPU's outputs to its monitor list.
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
