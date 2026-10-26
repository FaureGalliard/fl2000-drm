# FL2000 USB→HDMI driver — working context

Out-of-tree Linux kernel driver for USB-to-HDMI adapters based on Fresco Logic
FL2000DX + ITE IT66121 bridge. Forked from klogg/fl2000_drm (kernel 4.x era),
modernized for 6.x by Samuv5, ported to kernel 7.x here.

**Read `docs/DEVELOPMENT.md` before debugging anything** — it holds the hardware
map, the meaning of the driver's error messages, and the history of which
theories were tested and rejected. It will save you from re-deriving it.

## Current status (October 2026)

The display **works end-to-end**: EDID, modeset, PLL, USB streaming and HDMI
output all function, and GNOME renders the desktop on the external monitor at
1920x1080.

The driver is being **prepared for submission to the mainline kernel**
(dri-devel / drm-misc). The sources now live in `drivers/gpu/drm/fl2000/`
exactly as they would in the kernel tree, were cleaned up to kernel style, and
got several robustness fixes. `docs/UPSTREAMING.md` (Spanish) is the
step-by-step submission guide; `scripts/make-upstream-series.sh` turns the
repo into a patch series against a drm-misc-next checkout.

**None of the upstream-preparation commits, nor the earlier IRQ-starvation
fix (`082a6a4`), have been run on hardware yet.** They compile cleanly (W=1)
and pass `checkpatch --strict`. Running the field test on the adapter is the
immediate next task — see "Open issues".

## Environment

- Target: Arch Linux, kernel 7.1.x, and upstream drm-misc-next. The code uses
  current APIs only (`kzalloc_obj()`, `devm_drm_bridge_alloc()`, 3-arg
  `bridge->attach`, `i2c_algorithm.xfer`) and has **no version guards** —
  upstream does not accept them. Kernels older than 7.1 are not supported.
- Test hardware: UGREEN USB 3.0→HDMI dongle (`1d5c:2000`), LG 22MP55 monitor,
  HP Pavilion 15-cw1xxx with amdgpu, GNOME on Wayland.
- The dongle also contains an internal USB2 hub and a C-Media audio chip; they
  enumerate as separate devices. Do not confuse them with the FL2000.

## Build and install

```bash
make clean && make
sudo cp fl2000.ko /lib/modules/$(uname -r)/extra/ && sudo depmod -a
```

The top-level `Makefile` is only an out-of-tree wrapper: it builds
`drivers/gpu/drm/fl2000/` (Kconfig symbol `CONFIG_DRM_FL2000`) and copies
`fl2000.ko` to the repository root. Edit sources only under `drivers/`.

`it66121` is built **into** `fl2000.ko` — there is no separate `it66121.ko`.
Reloading with `rmmod` fails while a compositor holds the DRM device open; the
reliable path is to install the module and reboot with the adapter connected.

## Testing

```bash
sudo bash scripts/fl2000-field-test.sh     # from a TTY (Ctrl+Alt+F3)
```

Captures module/USB/DRM state, connectors, modes, EDID, a modeset color-bar
test and dmesg into `docs/field-test-HH_MM.log`.

The DRM driver name is `fl2000` (it was `fl2000_drm`): `modetest -M fl2000`.
Success-path messages (bind, EDID read, mode set) are `dev_dbg`/`drm_dbg`, as
upstream requires quiet drivers. To see them, boot with
`/etc/modprobe.d/fl2000-debug.conf` containing `options fl2000 dyndbg=+p`
(and `drm.debug=0x04` on the kernel command line for the DRM ones).

**Boot with the adapter already connected and the monitor powered on.** Hot
replug frequently fails: only the dongle's USB2 half re-enumerates and the
FL2000's SuperSpeed function never returns to the bus. If you must replug,
leave it unplugged ~10 s first and verify with `lsusb -d 1d5c:2000`.

## Non-obvious invariants — breaking these has caused real crashes

- **Never do heavy work while holding `stream->list_lock`.** It is taken with
  `spin_lock_irq()`, so multi-MB copies/conversions under it disable interrupts
  for milliseconds and starve the whole system (amdgpu logs `Fence fallback
  timer expired`, the desktop turns laggy). Own the buffer off the lists, do the
  work unlocked, then re-insert. Watch for `spin_unlock()` mispaired with
  `spin_lock_irq()`.
- **Never call `component_del()` (i.e. `it66121_destroy()`) from the master
  unbind path.** `component_master_del()` holds the global component mutex
  across the unbind callback → self-deadlock, hard hang.
- **The IT66121 component must live as long as the USB device**, not per bind
  cycle. Destroying and re-creating it leaves dangling pointers in the aggregate
  match data and Oopses in `component_bind_all()` on the next bind.
- **Framebuffers must use GEM SHMEM, not GEM DMA/CMA.** A 1080p buffer needs
  ~8.3 MB physically contiguous, which cannot be allocated on x86 without CMA.
  The device never DMAs the framebuffer — the CPU repacks it into URB buffers.
- **`bridge->dev` is NULL while the bridge is detached.** Use `&priv->client->dev`
  in the polling work and logging.
- **EDID comes from the direct FL2000-bus DDC path**, not the IT66121 EDID FIFO
  (which always times out on this board). Both paths are kept; the FIFO one is
  tried first and fails, then the direct one succeeds. See `docs/DEVELOPMENT.md`.
- **URB completion handlers run in atomic context.** No `GFP_KERNEL`, no
  `usb_clear_halt()`, no `cond_resched()` there; use `spin_lock_irqsave()`.
  `fl2000_submit_urb()` sleeps and is for process context only.
- **Register fields are `GENMASK()`/`FIELD_PREP()`**, never C bitfields
  (implementation-defined layout; one bitfield union silently read the wrong
  byte for years, see `docs/DEVELOPMENT.md`).

## Open issues

1. **Field-test the current branch on hardware** (highest priority). It
   carries the unverified IRQ-starvation fix plus all upstream-preparation
   commits. Success looks like: everything that worked before still works
   (EDID, 1920x1080 desktop, clean disconnect), no system-wide lag while the
   display is active, and `dmesg | grep -i fence` no longer filling with
   amdgpu fence fallback messages. Only then send the RFC
   (`docs/UPSTREAMING.md`).
2. **Frame pacing.** `drm_crtc_handle_vblank()` is called from every URB
   completion, so vblank runs at the USB transfer rate rather than the mode's
   60 Hz, and the stream free-runs at maximum USB rate instead of pacing to the
   refresh rate. This is a candidate cause if lag or flicker persists after (1).
   Not yet touched — measure before rewriting the timing.
3. The IT66121 EDID FIFO path never succeeds on this hardware (harmless: it
   costs ~500 ms of timeouts before the direct path runs, and it is kept because
   other board wirings may need it).
4. `sysfs` `edid` file has read empty while the connector property blob was
   populated. Unexplained, cosmetic so far.
5. **htotal adjustment never ran** (`s = 0` in the `fl2000_mode_calc()` loop):
   modes the PLL cannot hit exactly are rejected, which is why 1920x1080@60.00
   is unavailable and 59.94 Hz is used. The fix is ready in
   `docs/pending/0001-fix-actually-try-the-htotal-adjustments-when-searchi.patch`
   but changes the mode the monitor gets, so apply it only with the hardware
   at hand.
6. **Upstream review items** (expected requests, not started because they
   change working behavior): reuse the in-tree `ite-it66121` bridge instead of
   `fl2000_it66121.c`, drop `drm_simple_display_pipe`, `drm_bridge_connector`
   + `DRM_BRIDGE_ATTACH_NO_CONNECTOR`, a vblank timer instead of per-URB
   vblank (overlaps with 2), fbdev emulation, real suspend/resume, atomic
   bridge callbacks. Listed in `docs/UPSTREAMING.md` section 6 and asked about
   in the RFC cover letter.

## Workflow notes

- Field test logs are committed under `docs/` — that is how test results travel.
- The driver is loaded automatically at boot when the adapter is present
  (`depmod` registers its VID/PID), so a cold boot exercises the probe path.
- Keep commit messages in the conventional-commit style already used, and do not
  add co-author trailers (the maintainer publishes this fork under their own
  name, with upstream attribution kept in the README). The upstream kernel
  patch uses kernel style instead (`drm/fl2000: ...`, see
  `docs/upstream/commit-message.txt`).
