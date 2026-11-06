# Development notes — hardware map and debugging history

This file records *why* the driver looks the way it does, so the same dead
ends are not explored twice.

## Hardware map

```
USB 3.0 ──▶ FL2000DX ──┬── I²C master (regmap over USB control transfers)
                       │        │
                       │        ├── IT66121 HDMI bridge  @ 0x4C
                       │        └── monitor DDC/EDID     @ 0x50   ← EDID comes from here
                       │
                       ├── bulk EP1 OUT ── pixel stream ──┬──▶ IT66121 ──▶ HDMI
                       │                                  └──▶ DAC ───────▶ VGA (DDC at 0x50)
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

## VGA output (November 2026)

The UGREEN dongle has an HDMI and a VGA connector; the chip's DAC drives the
VGA one from the same pixel stream. `fl2000_vga.c` adds a `DRM_MODE_ENCODER_DAC`
encoder and a `DRM_MODE_CONNECTOR_VGA` connector on the one CRTC, the two
encoders being clones of each other. Measured on the real adapter with
`scripts/fl2000-reg.py` and an LG 19EN43 (1366x768, old, flaky EDID):

- **Status register 0x8000 bit 29 (`MONITOR_STATUS`) follows the VGA cable**
  (0x280000c1 with the cable, 0x08000000 without), and the chip powers its PLL
  and DAC by itself while it is set (ACLK bit 31, `PLL_DAC_PD_NOVGA_EN`, is set
  at reset): no power bit has to be written. Bit 27 (`HDMI_STATUS`) is a board
  strap and reads 1 with nothing on HDMI. Other boards' reports differ: on an
  HDMI-only board bit 29 follows the HDMI sink, on a VGA-only dongle it never
  moved. So there is **one monitor sense input**, and the connector logic is
  "sensed and the IT66121 hot plug line is low" = a VGA monitor. A monitor on
  VGA is therefore not reported while one is on HDMI; `video=VGA-1:e` forces it
  for clone mode.
- Driving only VGA with `FORCE_VGA_CONNECT` (ACLK bit 25) set would hide the
  cable being unplugged, so it is only set while the HDMI encoder is active and
  cleared when the CRTC is disabled. HDMI has always run with it set.
- Reading 0x8000 clears its event bits. Everything that reads it goes through
  `fl2000_intr_read_status()`, which latches the events until the interrupt work
  reports them; `scripts/fl2000-reg.py watch` steals events while it runs.
- **The EDID of the LG monitor cannot be read at 0x50** (I²C status 15, NAK)
  even while the chip's own EDID detector reports a monitor (status bit 28) and
  an image is shown. Old monitors often have no usable EDID; Windows needed
  Custom Resolution Utility for it. Without an EDID the standard timings are
  offered with 1024x768 as the default. Native modes can be forced with
  `video=VGA-1:1366x768@60e` or replaced with `drm.edid_firmware=VGA-1:edid/x.bin`
  (the same job CRU does on Windows). Untested ideas for the NAK: the chip's
  periodic detection colliding with the read (`fl2000-reg.py edid-quiet`), a
  monitor that only answers DDC for a short while after the cable is plugged
  (`edid-wait`), or DDC not wired in that cable.
- **PLL search**: it had a loop meant to stretch htotal by a few pixels until the
  clock landed on a PLL setting, which never ran (the sign started at 0). It is
  gone: only the pixel clock moves, by up to 5000 ppm on VGA
  (`FL2000_PPM_ERR_MAX_VGA`, as Samuv5's driver does) and 500 ppm for digital
  sinks (`fl2000_hdmi_encoder_mode_valid()`, identical to the old behaviour for
  1200 of 1200 random modes). 85.5 MHz modes get 85.454 MHz (531 ppm off).
- **Hardware results with the LG 19EN43 (1366x768 panel, no readable EDID)**, new
  module, session of 5 Nov 2026:
  - Work: 1024x768, 1280x720, 1280x800, 1280x768 (colours normal; the washed-out
    look seen while GNOME asks "keep changes?" is the dim layer of that dialog,
    it goes away on Keep).
  - "Out of range" on the monitor: 1440x900, 1280x1024, 1600x900. Expected: they
    exceed the panel.
  - **1366x768: plain black screen, no OSD message. 1360x768: most of the screen
    garbled or black.** The chip's timing side is fine: `fl2000-reg.py diag`
    counts 59.5 Hz against 59.76 programmed, PLL and timings as computed, and the
    timings equal the vendor's table entry (htotal 1792x798, hsync 143, hstart
    357, vstart 28); only the vendor's pixel clock differs (85.83 MHz, 60.02 Hz).
  - **Rejected by `fl2000-reg.py sweep` on the running 1366x768 mode** (all six
    variants stayed black): the four combinations of the sync polarity bits
    (0x8004 bits 2/3) and the vendor's pixel clock with two polarities. An exact
    htotal does not help either (the previous module changed it, this one does
    not). The monitor was also reported to need CRU on Windows.
  - **Cause of the black 1366x768: the frames had no end.** The device takes a
    zero-length packet as the end of a frame (ACLK 0x803C bits 27-29 select the
    end-of-frame type; bit 28, `USE_ZERO_PKT_LEN`, is the vendor's
    `EOF_ZERO_LENGTH`). The driver relied on `URB_ZERO_PACKET`, which only sends
    that packet when the transfer is a whole number of packets. A 1366x768 frame
    is 3147264 bytes, 3073.5 packets of 1024: it ended in a short packet and
    nothing followed. The vendor driver sends a separate zero-length URB after
    every frame and no `URB_ZERO_PACKET` (`fl2000_bulk.c`, `fl2000_render.c` in
    github.com/FrescoLogic/FL2000); the driver now does the same (commit
    627c569). **Verified on hardware on 5 Nov** (`field-test-19_03.log`): every
    test mode now runs at one frame per refresh (1366x768 at both clocks 59/60
    sent/s, 800x600 and 848x480 60), and the colour bars show at 1366x768
    (both), 1360x768, 800x600 and 1024x768; 848x480 gave 'no signal', which with
    the device taking its frames normally points at the monitor. Every mode whose frame is not a
    multiple of 1024 bytes was affected at 24 bpp, including 800x600, 848x480,
    1400x1050, 1440x900, 1600x900 and 1680x1050; 1024x768, 1280x720/768/800,
    1360x768 and 1920x1080 are multiples and were not. The line-padding idea
    (widths that are multiples of 64 pixels) is not needed for this.
  - **How it showed: the `Stream:` line.** When the device takes our frames it
    accepts exactly one per refresh (USB flow control paces the bulk endpoint),
    so `sent/s` equals the refresh rate: 60 at 1024x768, 1280x720 and 1360x768,
    59 at 1920x1080@59.94. With the frame end missing it took data at exactly 3
    bytes per pixel clock, blanking included: 81 sent/s and 244 MB/s (MiB) at
    1366x768@59.79 (85.454 MHz), 68 sent/s and 206 MB/s at 1366x768@60 (72 MHz),
    68 sent/s and 253 MB/s at 1440x900 (88.75 MHz), all within 0.5 % of 3 x
    pixel clock. `sent/s` well above the refresh rate therefore means the device
    is not seeing frame ends, whatever the monitor shows; the field test prints
    the last `Stream:` line after each test mode for this reason.
  - **Fixed (ea3ea23, verified 5 Nov): GNOME at 1360x768 and 1366x768** showed
    only the bottom ~10 %. `scripts/fl2000-dump-fb.c` proved the compositor's
    buffer was right (AR30 imported, pitch 5632). With the shmem no-map import
    amdgpu kept it in VRAM and `ttm_bo_vmap()` mapped one linear range placed
    to end at its highest VRAM block, so a buffer split over blocks read right
    only at the end. `drm_gem_prime_import()` maps the attachment, amdgpu pins
    the buffer in GTT, and the picture is whole: 60 sent/s, 8.7 ms per full
    frame, still zero-copy. History of the symptom: GNOME at 1366x768@59.79
    (85.5 MHz) showed only the
    bottom ~10 % of the desktop correctly; the rest is a stable black area that
    distorts when a window is dragged over it, and the monitor's AUTO button
    does not help. The colour bars at the same mode from modetest looked right,
    so compare GNOME at 1366x768@60.00 (72 MHz, reduced blanking) and the
    framebuffer GNOME hands over (pitch, modifier, imported or not) next.
  - **1360x768 was a different problem.** Its frame is a whole number of packets
    (3060 x 1024), the device ran at one frame per refresh, and the picture was
    still mostly garbled, so the ZLP fix does not change it on the wire. Suspect
    the monitor first: 1360x768 and 1366x768 share htotal 1792, the pixel clock
    and the sync frequencies, so it may sample one as the other (try its AUTO
    button). 848x480 (53 x 16 pixels, not a multiple of 32 like 1360) separates a
    chip limit from the monitor once the ZLP fix is in.
  - At 1366 pixels one full-frame conversion took ~78 ms instead of ~8 ms: the
    line of 5464 bytes is not a multiple of 16, so `drm_memcpy_from_wc()` fell
    back to a plain memcpy from write-combined memory. `fl2000_read_span()` now
    rounds the length up to 16 bytes when the source is aligned (commit
    b22b48e); expect `conv` ~8 ms per full frame at 1366x768.
  - The colour-bar test from a TTY separates GNOME's framebuffer path from the
    driver and the monitor. It must run on a real VT (Ctrl+Alt+F3): from a
    terminal inside GNOME the mode set fails with `Permission denied` because
    Mutter is the DRM master. The three logs `field-test-18_2[789].log` of 5 Nov
    were taken that way (`TTY=pts/1` in the journal's sudo lines) and show no
    picture; their `Stream:` lines are what exposed the missing frame end.
  - The EDID at 0x50 returns I2C status 15 (NAK) at offset 0 (once at offset 4);
    `edid-quiet`, `edid-wait` and a replug of the USB did not change it.
- **Odd line widths were scrambled** at the native 1366x768 of that monitor: the
  converters swapped the 32-bit halves of the 64-bit words from the start of
  each *line*, which is only right when a line is a multiple of 8 bytes (4098
  bytes at 1366 pixels). The swap now follows the offset in the frame, and
  `scripts/test-frame-layout.py` checks the real functions against an
  independent conversion (about half the bytes were wrong at 1366 and 854).

Known limits: adapters with only a VGA connector and no IT66121 are not
supported (the driver needs the bridge to bind); the DAC pixel clock is capped
at 165 MHz by policy since there is no datasheet; the polarity of the sync
bits (0x8004 bits 2/3) is never programmed (they reset to 1) and may matter to
very old monitors.

## HDMI connector from drm_bridge_connector (5 November 2026)

The IT66121 bridge no longer creates its connector. It attaches with
`DRM_BRIDGE_ATTACH_NO_CONNECTOR`, declares `DRM_BRIDGE_OP_DETECT |
DRM_BRIDGE_OP_EDID` and type HDMI-A, and `fl2000_drm_bind()` builds the
connector with `drm_bridge_connector_init()`, as `Documentation` asks of new
display drivers (`drm_bridge.c`, "new model"). What changed in behaviour, and
what the hardware test has to look at:

- **The mode is programmed in the bridge's `atomic_enable`**, not in
  `.mode_set`: HDMI or DVI comes from `connector->display_info.is_hdmi` of the
  connector found in the atomic state, as in the in-tree `ite-it66121`. The
  atomic helpers enable the CRTC (stream start) before the bridge, so the
  IT66121 is now set up while pixels already flow; before, it was set up before
  the stream started. A monitor without a readable EDID now gets DVI mode (it
  used to get HDMI mode with an infoframe).
- **The HDMI connector is polled every 10 s** (`DRM_CONNECTOR_POLL_CONNECT |
  DISCONNECT`): one HPD register read on the shared I2C bus. Before, it was
  only probed on hotplug events and by userspace. No `DRM_BRIDGE_OP_HPD`: the
  IT66121 interrupt reaches the host through the FL2000 interrupt endpoint,
  which still sends a device-wide hotplug event (`fl2000_drm_hotplug()`).
- **The EDID cache stays in the bridge**: `.edid_read` hands out a copy and
  reads again only when `.detect` saw HPD low, so the one-read-per-connection
  fix for the desktop stall is kept. The connector calls `.detect` before every
  EDID request.
- **Out-of-tree builds compile the helpers into the module** on kernels built
  without `CONFIG_DRM_BRIDGE_CONNECTOR` (Arch 7.2.11 lacks it and the HDMI
  state, audio and CEC helpers it calls). The four files are the kernel's own,
  unchanged (`compat/drm-helpers/7.2/`, from v7.2.11); they compiled without a
  warning and resolved every symbol, `fl2000.ko` then depends on `cec` and
  `drm_display_helper`, and none of their 15 exported symbols exists in the
  running kernel. 7.2 and 7.4-rc1 `drm_bridge_connector.c` differ only for
  `DRM_BRIDGE_OP_HDMI` bridges, so the Arch test stands for 7.4.

HDMI check of the same day, before this change (module 9ECE56DA, with the
mapped import): 1920x1080 on the LG 22MP55 at 59 sent/s, and GNOME switched
from VGA to HDMI and back without logging out. One sample gave 19.9 ms per
full-frame conversion at 1080p, against ~9 ms from VRAM on 27 Oct: the
compositor's buffer now sits in GTT. Plenty for a desktop; re-measure while
dragging a window before drawing conclusions.

## Suspend and resume (5 November 2026)

The callbacks used to do nothing, and there was no `reset_resume`. The test
laptop sleeps in S3 (`/sys/power/mem_sleep` is `deep`) and its journal shows
every USB device being reset on wake; for a reset-resume without the callback
the USB core unbinds and binds the driver, which a running compositor takes as
an unplug (see "Unplug and replug" for what Mutter does with that). Earlier
suspends in the journal were all without the dongle, so the old behaviour with
it was never observed.

Now (commit bc351ed, verified on 5 Nov with `pm_test=devices` and a real S3 cycle in which the dongle was reset-resumed; the picture came back without errors):

- the USB core calls the driver once per interface, the last first on
  suspend and in order on resume; `fl2000_drv.c` stops the display at the
  first suspend and starts it at the first resume, and `reset_resume` is the
  same callback;
- suspend: kill the interrupt URB, `drm_mode_config_helper_suspend()` (state
  saved, outputs off, stream drained);
- resume: drop both register caches (a cached bit that looks set would make
  a read-modify-write skip a write the chip needs after a power loss),
  re-run the IT66121 initialisation of attach and the FL2000 reset and USB
  setup of bind, submit the interrupt URB, mark both EDIDs stale,
  `drm_mode_config_helper_resume()`, and send a hotplug event.

The core restores the interface altsetting after the reset of a reset-resume,
so the streaming interface is not set again by the driver.

## Review of the RFC (6 November 2026)

The Sashiko AI review answered the RFC within the hour with 32 findings. Three
were duplicates of others and the rest were checked one by one against the code
and the kernel sources. Nearly all were real, with some severities and scenarios
off. What it found, and what stands now:

- **`it66121_i2c_probe()` never looked at the chip.** The callback of
  `i2c_new_scanned_device()` returns nonzero for "found", and it returned 0 for
  found and -ENODEV for not found. It also checked for
  `I2C_FUNC_SMBUS_READ_BYTE`, which the adapter does not have, so it always
  returned -ENODEV before any transfer: the first address was always "found",
  with or without a chip. Our own logs had it all along (`Adapter does not support
  I2C functions properly` right before a working bridge,
  `docs/field-test-15_04.log`). The read now really runs, for the first time on
  hardware, and at the very start of the probe. If an adapter does not come up,
  look here first: `No IT66121 found` in the log.
- **The EDID FIFO path is gone.** See "Decoding the driver's DDC messages". Each
  connection saved ~650 ms and a `dev_err`. The abort stays, since it was always
  what ran before the first direct read.
- **The lifetime model changed**: one binding, on interface 0, and the other two
  interfaces claimed from its probe. Shared state on the `struct usb_device` is
  gone: that device stays bound to `usb_generic`, so `rmmod` or a sysfs unbind
  released nothing and the next unplug or load crashed. The rules this replaces
  (component must outlive bind cycles, never `component_del()` from unbind) went
  with the component framework; the crashes they prevented in the table below
  are the old model's. `.pre_reset`/`.post_reset` keep a USB reset (the storage
  function's error handler asks for them, see Hardware map) from looking like an
  unplug to the compositor.
- **An open DRM file may outlive the binding.** The stream is `drmm` managed, so a
  commit still on its way finds it; everything that touches a register goes through
  `drm_dev_enter()` or, in the bridge, `drm_bridge_enter()`. A new binding gets a
  new bridge, so the old device held open does not make the attach fail with -EBUSY.
- **The IT66121 is no longer polled.** Ten I2C transactions a second acknowledged
  interrupts nobody looked at. It is acknowledged when the FL2000 reports
  `HDMI_EVENT`, and hot plug is unmasked, so a monitor should now show up at once
  instead of within the 10 s of the connector polling, which stays as the net. Not
  seen on hardware yet whether the line really reaches the FL2000: if not, nothing
  is lost.
- **`FORCE_VGA_CONNECT` hides the sense**, so it counts as no monitor. Unplugging
  HDMI while driving it reported an empty VGA connector as connected until the
  mode set on it cleared the force. The earlier tests hid it, plugging VGA right
  after.
- **The cursor is blended as premultiplied.** The formula was for straight alpha,
  and compositors hand over premultiplied sprites: antialiased edges came out too
  dark. Not found by the review; it asked for the blend mode property, which the
  core now warns about (`pixel format with alpha exposed but blend mode not
  setup`, drm-misc-next only: 7.2.11 has no such check).

How it was tested, since the adapter was not at hand: a fake FL2000 + IT66121 in
userspace, on `raw_gadget` and `dummy_hcd` in a VM (branch `test/vm-harness`,
`scripts/vm-test/`). It answers the register, I2C and EDID traffic and checks the
frames that reach the bulk endpoint byte by byte. It ran on two kernels, with the
driver unmodified: drm-misc-next with KASAN, lockdep, kmemleak and fault injection,
and the real Arch 7.2.11 with the very `fl2000.ko` that gets installed, compat
helpers included. About 100 checks pass on each (`docs/vm-test-v2.log`): frames for
every format and width, damage clips and framebuffers larger than the mode, cursor,
legacy and atomic page flips at the refresh rate, hot plug, unplug while streaming
with the file open, replug, `rmmod` and then unplug, loading with the adapter
present, sysfs unbind and bind of any interface, rebinding with the old device held
open, a USB reset while streaming, unplugging in the middle of the probe, storms of
commits during unbind, allocation failures all over the mode set, and no leaks. The
same tests against the v1 driver crash it: `rmmod` then unplug, the huge mode, and a
zero-length I2C message. None of this says anything about the real chip: the timings,
the DAC, the IT66121 itself, the amdgpu import or GNOME. Those need the adapter.

One thing seen there and left alone: `vblank wait timed out on crtc 0` from
`drm_fb_helper_damage_work` if something is writing to the fbdev, with no compositor,
at the moment of the unplug. The worker took its vblank reference just before the
shutdown turned the CRTC off, in the core, and the wait then runs out.

What was checked and rejected or left alone:

- *A huge mode "with only the cursor plane enabled"*: `drm_crtc_helper_atomic_check()`
  requires a primary plane. The bug is real all the same, with the primary plane
  attached and placed off screen: an invisible plane returns before the "must cover
  the CRTC" check. `drm_mode_validate_size()` only runs on probed modes.
- *The xHCI temporary buffer path for `urb->transfer_buffer`*: only some
  `xhci-plat` controllers have the quirk, and page-sized scatterlist entries do not
  trigger it. KMSAN and host controllers with local memory do look at the field,
  and it was a hack anyway.
- *`urb->interval`*: harmless on xHCI, which uses its endpoint context. Nothing has
  to be restored at all.
- *The in-tree `ite-it66121`*: left to the maintainers' answer to question 1 of the
  cover letter (`docs/pending/it66121-in-tree.md`).

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

The driver no longer uses the EDID command of this engine (see "Review of the
RFC"), only the abort that releases the bus, once after each chip reset. That
abort has always completed, so `DDC not done` in the debug log is now a surprise.

## Bug history (symptom → root cause)

Each of these was a real failure observed on hardware, in the order they were
found. All are fixed. The component and `usb_device` devres rows describe the model
that was replaced in November 2026 (see "Review of the RFC").

| Symptom | Root cause |
|---|---|
| Hard kernel hang on disconnect | `it66121_destroy()` → `component_del()` called from the master unbind callback, which already holds the global component mutex → self-deadlock |
| Machine hangs when the adapter is unplugged while streaming | A data URB whose submission failed (`-ENODEV`) was freed while still on the stream's anchor; `usb_kill_anchored_urbs()` then spun forever on the dangling entry. Fixed with `usb_unanchor_urb()` |
| After an unplug the dongle never enumerates again (`usb_hub_wq` stuck in `i2c_del_adapter()`) | The DRM device was a child of the I2C adapter and so held a reference on it, while `i2c_del_adapter()` waits for all references. The DRM device is now a child of the USB device and `drm_dev_put()` is explicit in `fl2000_drm_if_release()` |
| After a monitor/cable cycle the connector only offers modes up to 1024x768 and an empty EDID | The EDID was cached in the bridge and only handed to the connector the first time; the DRM core drops it when the connector goes through `disconnected`. It is now handed to the connector on every probe and read again only after HPD reported no monitor (reading it on every probe stalled the compositor for ~1 s per probe), starting with the DDC path that worked last time |
| With Mutter 51 the desktop stalls after ~30 min: `amdgpu_dm_plane_helper_prepare_fb ... bind failed`, page flips fail with ENOSPC | Mutter renders ARGB2101010 and tries to scan that buffer out on our plane; AddFB2 failed with EINVAL (`Zero-copy disabled ... format=AR30`) because only XRGB8888 was advertised, so Mutter used its primary-GPU copy path (Mutter issue #5101, a dma-buf leaked per frame) until amdgpu's GTT was full. XRGB2101010/ARGB2101010 are now accepted and converted to 8 bits per channel; needs a long-run check of `mem_info_gtt_used` |
| Image scrambled or skewed at widths such as 1366 or 854 (1920, 1024 and 1360 fine) | The 64-bit word swap of the frame was applied per line; see the VGA section. Fixed with frame offsets |
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
| `fl2000_get_bytes_pix()` divides a bandwidth in bytes/s by the pixel clock in **kHz** (callers pass `drm_display_mode.clock`), and the "80 % of USB bandwidth" macros compute `* 100 / 80` (125 %). The result is always ≥ 3, so every mode on every USB speed streams RGB888 and the bandwidth check never rejects anything | **Fixed 5 Nov 2026** (150722c). The question left open here was answered by the frame-end work: the device takes exactly one frame per refresh and has no frame buffer, so data cannot arrive slower than the refresh rate. The check now uses the active pixel rate against 450 MB/s (SuperSpeed: 500 MB/s after 8b/10b, 90 % usable) or 48 MB/s (high speed: 13 x 512 bytes per microframe, 90 %). USB 3.0 keeps RGB888 up to 1920x1200@60 (415 MB/s, which the vendor offers in RGB888) and uses RGB565 for 2560x1080@60; USB 2.0 only fits 640x480 in RGB565 (the vendor compresses there). RGB565 itself is not yet tested on hardware |
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

Research of 5 Nov 2026 (two read-only agents, plus the journal of every unplug):

- Unplug crashes of gnome-shell are intermittent with the *same* module: 3 of
  about 10 unplugs between 27 Oct and 5 Nov (`libmutter` null dereference at
  0x18 in the KMS thread, or 0x7c in the main thread), the rest clean, so no
  driver version avoids it. Mutter 51.1 has also crashed once with no unplug.
- Mutter has no fix and no open work for removing a GPU object (#710, #17); the
  only merged change (!713) stops the crash on removal but keeps the object.
  gud (notro/gud#13) and the other FL2000 driver (vfox888-cpu/alpha-fl2000)
  report the same: a replug works only when the kernel hands out a new node,
  and the advice is to log out before unplugging.
- Kernel side: DRM minors are not recycled "immediately" according to
  `Documentation/gpu/drm-uapi.rst`, but `drm_minor_alloc()` uses `xa_alloc()`
  and reuses the lowest free number (the number is released at the last
  `drm_dev_put()`). A cyclic allocation would be a small core change in line
  with the documentation; nobody has proposed it and it is not known whether it
  would help (it depends on whether Mutter's key is the path or the open fd).
  A driver may mark the connector disconnected and send a hotplug event before
  `drm_dev_unplug()` (the uAPI documents it); udl, gud and gm12u320 do not.
  Keeping the DRM device alive across an unplug contradicts the uAPI text.
- The interrupt work could run after `drm_dev_unplug()` and send a hotplug
  event for a removed node; it is now stopped first.

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
  board regardless of timing; the wiring is different (see Hardware map). It was
  removed in November 2026: behind this I2C engine, which reads whole dwords, each
  poll of the DDC status (0x16) also pops the DDC FIFO (0x17).

## Verifying kernel APIs

`git.kernel.org` blocks automated fetches (Anubis). Use the GitHub mirror:

```
https://raw.githubusercontent.com/torvalds/linux/v7.1/<path>
```

Download headers locally and grep them rather than trusting search results or
recollection — several of the fixes above came from reading
`drivers/base/component.c` and `drivers/gpu/drm/drm_bridge.c` directly.
