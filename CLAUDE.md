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
immediate next task — see "Open issues". The October 2026 API migration
(simple pipe → atomic objects, shadow plane + format helpers, fbdev, vblank
timer, atomic bridge callbacks) changes runtime behavior: test commit by
commit if something regresses (`git log --oneline 9216213..`).

The race is on: klogg, rhgndf (Jie Feng), Samuv5 and vfox888 all have active
FL2000 drivers (`docs/COMPARISON.md`). The goal of this fork is to follow the
newest kernel API so it is the one maintainers can take.

**Coordinate with the original author before posting anything.** Artem
Mygaiev (klogg) resumed work in Oct 2026: he dropped simple-pipe, adapted the
in-tree IT66121 driver, and said in klogg/fl2000_drm#86 that he will upstream
the IT66121 work himself. See `docs/UPSTREAMING.md` section 0b, and that
file's timeline section: the earliest realistic mainline release is 7.6 (late
April 2027), not January.

## Environment

- Target: upstream **drm-misc-next / Linus master (7.4 cycle)**, and Arch
  Linux 7.1.x for testing. The driver source follows the newest API
  (`struct drm_atomic_commit`, bridge `atomic_create_state`, `kzalloc_obj()`)
  and has **no version guards** — upstream does not accept them. Older kernels
  build only through the out-of-tree wrapper: the top-level `Makefile` probes
  the target headers and, if they use older names, copies the sources to
  `.compat-build/` and translates them with the sed rules in `compat/*.sed`
  (the technique of the kernel backports project; see `compat/README`).
  `compat/` is never part of the upstream patches. If a future rename breaks
  the 7.1 build, add a sed rule, never an `#if` in the driver. Kernels older
  than 7.1 are not supported.
- The upstream series is generated and build-tested against **Linus master**
  (`/home/user/linux-master` in the cloud workspace), not v7.1: the driver
  source does not compile on 7.1 untranslated.
- Re-check against Linus master before each submission. Legacy bridge
  `.enable`/`.disable` were removed in the 7.4 cycle, and the driver stopped
  compiling there until it moved to `atomic_enable`/`atomic_disable`.
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
- **The IT66121 I2C client must not be named `"it66121"`.** That is the
  in-tree `ite-it66121` driver's `i2c_device_id`. If that driver is built, it
  would try to bind our client, fail without an OF node, and release our
  devres (use-after-free). Ours is `"fl2000-it66121"`, matched in
  `fl2000_compare()`.
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
2. **Frame pacing.** Vblank now comes from the DRM vblank timer at the mode's
   refresh rate (it used to follow every URB completion). The stream itself
   still free-runs, resending the last frame at maximum USB rate; pacing it to
   the refresh rate would save USB bandwidth and CPU. Measure first.
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
6. **USB bandwidth check is a no-op**: `fl2000_get_bytes_pix()` gets the
   pixel clock in kHz but treats it as Hz (and the 80 % margin is inverted),
   so RGB888 is always chosen. Harmless for 1080p60 on USB 3.0, wrong for
   USB 2.0 and for modes above ~166 MHz. Details in `docs/DEVELOPMENT.md`;
   needs hardware to decide the right behavior on USB 2.0.
7. **Odd line widths** (1366 px) were scrambled by the per-line 64-bit half
   swap; fixed by the move to the format helpers + whole-buffer swap, but a
   1366x768 mode has never been checked on hardware.
8. **Upstream review items still open** (done: simple pipe removed, shadow
   plane, format helpers, vblank timer, fbdev, atomic bridge enable/disable):
   - `drm_bridge_connector` + `DRM_BRIDGE_ATTACH_NO_CONNECTOR`. Blocked for
     the out-of-tree build: `CONFIG_DRM_BRIDGE_CONNECTOR` is only selected by
     ARM drivers, so x86 distro kernels may lack `drm_bridge_connector_init()`
     (klogg vendors the helper sources for that). Check with
     `zgrep DRM_BRIDGE_CONNECTOR /proc/config.gz` on the test laptop first.
   - reuse the in-tree `ite-it66121` bridge (asked in the RFC cover letter)
   - no component framework (`drm_bridge_attach()` from probe), a single
     interface plus `usb_driver_claim_interface()`, DRM device parented to the
     USB interface with `drm_dev_set_dma_dev()`. This is the riskiest rework:
     it replaces the lifetime model behind the crash fixes in "Non-obvious
     invariants".
   - real suspend/resume (`drm_mode_config_helper_suspend()`), bridge
     `mode_set` → atomic hooks, `DRM_BRIDGE_OP_HDMI` infoframes.
   Listed in `docs/UPSTREAMING.md` section 6.
   `Documentation/process/coding-assistants.rst` asks for an `Assisted-by:`
   tag on AI-assisted kernel patches. `make-upstream-series.sh` adds it only
   when `ASSISTED_BY` is set; the maintainer decides.

## Workflow notes

- Field test logs are committed under `docs/` — that is how test results travel.
- The driver is loaded automatically at boot when the adapter is present
  (`depmod` registers its VID/PID), so a cold boot exercises the probe path.
- Keep commit messages in the conventional-commit style already used, and do not
  add co-author trailers (the maintainer publishes this fork under their own
  name, with upstream attribution kept in the README). The upstream kernel
  patch uses kernel style instead (`drm/fl2000: ...`, see
  `docs/upstream/series/`). The upstream series is 4 patches (USB/regmap/I2C,
  IT66121, streaming, KMS). If you change `fl2000_drv.c`, `fl2000.h` or the
  Makefile, regenerate `docs/upstream/stages/*.diff` (reduced versions of
  those files for patches 1-3); the script fails if they no longer apply or
  if the end of the series differs from `drivers/gpu/drm/fl2000/`. Use
  `CHECK_BUILD=1` to build-test every patch.
