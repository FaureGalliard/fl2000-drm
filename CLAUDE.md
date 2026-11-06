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

**Verified on hardware on 27 Oct 2026** (`integration` branch, commit
`8f93e98`): the upstream-preparation commits plus the performance work from
`main` (shadow frame, cursor plane, streaming loads, accepting imported
buffers). Clean boot, desktop and cursor on the external monitor, no
`fence fallback`, no `amdgpu bind failed`, 59 sent/s, up to 32 rendered/s
while dragging, ~9 ms per full-frame conversion, RGB888. If something
regresses, bisect `git log --oneline 9216213..`.

Since then (3-5 Nov 2026, kernel 7.2.11, Mutter 51.1): unplug no longer hangs
the machine, the 10-bit scanout formats Mutter renders in are accepted, the
DRM device hangs off the USB device, the EDID is cached per connection, and the
analog VGA output of the UGREEN dongle was added (it showed a stable GNOME
desktop through the HDMI connector forced on, before the VGA connector existed).
The upstream series was regenerated on drm-misc-next 7.4-rc1 and built per
patch before the VGA work; regenerate it again (`docs/UPSTREAMING.md`).

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
- The HDMI connector is `drm_bridge_connector`, which Arch (and most PC
  distribution kernels) build without. The wrapper compiles the kernel's own
  helper sources into `fl2000.ko` from `compat/drm-helpers/<major.minor>/`
  (`cec` and `drm_display_helper` become dependencies). For a new kernel
  series `make` downloads them with `compat/fetch-drm-helpers.sh`: commit
  the new directory. See `compat/README`.
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

**Install the module for every new kernel.** Out-of-tree modules live per
kernel version; after `pacman -Syu` brings a new kernel the driver is simply
missing until it is rebuilt and installed
(`sudo install -D -m 644 fl2000.ko /lib/modules/$(uname -r)/extra/fl2000.ko &&
sudo depmod -a`). The tree built unchanged for 7.1.8 and 7.2.11.

Hot replug re-enumerates and rebinds (a blocked `usb_hub_wq` used to prevent
that). Leave the dongle unplugged ~10 s first and verify with
`lsusb -d 1d5c:2000`.

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
- **The IT66121 bridge creates no connector** (`DRM_BRIDGE_ATTACH_NO_CONNECTOR`):
  `fl2000_drm_bind()` makes the HDMI connector with `drm_bridge_connector_init()`,
  and the bridge only reports `.detect` and `.edid_read`. The EDID cache lives
  in the bridge (it hands out copies; `.detect` marks it stale when HPD drops),
  and the mode is programmed in `atomic_enable`, where the connector that says
  HDMI or DVI is known from the atomic state. No `DRM_BRIDGE_OP_HPD`: the
  interrupt comes through the FL2000, so the connector is polled.
- **`bridge->dev` is NULL while the bridge is detached.** Use `&priv->client->dev`
  in the polling work and logging.
- **EDID comes from the direct FL2000-bus DDC path**, not the IT66121 EDID FIFO
  (which always times out on this board). Both paths are kept; the FIFO one is
  tried first and fails, then the direct one succeeds. See `docs/DEVELOPMENT.md`.
- **URB completion handlers run in atomic context.** No `GFP_KERNEL`, no
  `usb_clear_halt()`, no `cond_resched()` there; use `spin_lock_irqsave()`.
  `fl2000_submit_urb()` sleeps and is for process context only.
- **The DRM device must not be a child of the I2C adapter.** `i2c_del_adapter()`
  (run when the USB device is unbound) waits for every reference on the
  adapter; a DRM device parented there holds one, so the hub worker blocks
  forever and nothing enumerates again after an unplug. It is parented to the
  USB device (`__drm_dev_alloc()`, DMA device = the host controller) and
  released with an explicit `drm_dev_put()` in `fl2000_drm_if_release()`.
- **Every `usb_anchor_urb()` needs a `usb_unanchor_urb()` when the submission
  fails.** A freed URB left on the anchor makes `usb_kill_anchored_urbs()` spin
  forever (hang on unplug).
- **The IT66121 I2C client must not be named `"it66121"`.** That is the
  in-tree `ite-it66121` driver's `i2c_device_id`. If that driver is built, it
  would try to bind our client, fail without an OF node, and release our
  devres (use-after-free). Ours is `"fl2000-it66121"`, matched in
  `fl2000_compare()`.
- **Damage lands in the shadow frame, never in a chain of URB buffers.**
  `fl2000_stream_compress()` converts only the damaged rectangle into
  `stream->shadow`, and every outgoing buffer is brought up to date from it
  (replaying `damage_log`). Converting into whichever URB buffer was free made
  frame age depend on list membership and lost damage when no buffer was
  free — that left trails behind a dragged window.
- **The send path must never wait for a conversion.** `fl2000_stream_build()`
  uses `mutex_trylock(&shadow_lock)` and otherwise resends — but never a frame
  older than `newest_sb`, or the picture steps back in time. Blocking there
  stops URB submission and the monitor blinks.
- **The cursor is blended on the way out, not into the shadow**, so moving the
  pointer costs one 64x64 blend. Without a cursor plane of our own the
  compositor keeps the pointer on the primary GPU's hardware cursor, where it
  never reaches this output.
- **Accept imported framebuffers, and read I/O memory with streaming loads.**
  The compositor hands us the primary GPU's VRAM buffer; `memcpy_fromio()`
  reads it at ~100 MB/s (~88 ms per 1080p frame), `drm_memcpy_from_wc()`
  in ~8 ms. Refusing imports instead pushes mutter onto a CPU copy path that
  leaked ~1.7 GB/min of GTT until the whole desktop stalled. On cached RAM a
  plain memcpy is faster, so `fl2000_read_span()` chooses per source.
- **Import framebuffers with a mapping (`drm_gem_prime_import`), not the
  shmem no-map import.** Unmapped, amdgpu keeps the buffer in VRAM and vmaps
  it as one linear range, right only for its last VRAM block: GNOME at
  1360/1366x768 showed just the bottom ~10 %. Mapping makes amdgpu pin it in
  system memory. `scripts/fl2000-dump-fb.c` saves the buffer the compositor
  hands us, to tell compositor bugs from ours.
- **The word swap of the frame follows the offset in the FRAME, never in the
  line.** The FL2000 wants the 32-bit halves of every 64-bit word of the flat
  frame swapped; a line only starts on a word boundary when its length is a
  multiple of 8 bytes (1366 px at 3 bytes is 4098: half the bytes were wrong).
  Converters and the cursor blend take the frame base plus the line offset, and
  `fl2000_copy_rect()` copies whole words. `scripts/test-frame-layout.py` checks
  the real functions; run it after touching `fl2000_streaming.c`.
- **Every frame ends with a zero-length URB of its own.** The device takes a
  zero-length packet as the end of a frame (ACLK bit 28). `URB_ZERO_PACKET`
  only sends one when the frame is a whole number of 1024-byte packets, so
  1366x768 (3073.5 packets) ended in a short packet, had no end, and stayed
  black. The vendor driver sends the separate URB too. Symptom to recognise:
  `Stream:` reports `sent/s` well above the refresh rate (the device drains 3
  bytes per pixel clock); a working mode shows exactly one frame per refresh.
- **Reading status register 0x8000 clears its event bits**, so every read goes
  through `fl2000_intr_read_status()`, which latches them until the interrupt
  work reports them. Do not read the register anywhere else, and note that
  `scripts/fl2000-reg.py watch` steals events while it runs.
- **`FORCE_VGA_CONNECT` (ACLK bit 25) is only set while the HDMI encoder is
  active** and is cleared when the CRTC is disabled: set while driving VGA it
  hides the cable being unplugged and VGA detection would never see it go.
- **Do not change the timings of a mode to fit the PLL (htotal).** The PLL
  search only moves the pixel clock, which may be off by up to
  `FL2000_PPM_ERR_MAX_VGA` on VGA; HDMI keeps 500 ppm
  (`fl2000_hdmi_encoder_mode_valid()`), see `docs/pending/htotal-hdmi.md`. (A
  guess that a changed htotal broke 1366x768 on the LG 19EN43 was tested and is
  wrong: it still fails with the exact htotal.)
- **No hotplug uevent before `drm_dev_register()`**: the core dereferences the
  minor's device, which does not exist yet (`registered` flag in
  `fl2000_drm.c`). The VGA encoder is created after `component_bind_all()`
  because the IT66121 attaches to the first encoder, which must be the HDMI one.
- **Register fields are `GENMASK()`/`FIELD_PREP()`**, never C bitfields
  (implementation-defined layout; one bitfield union silently read the wrong
  byte for years, see `docs/DEVELOPMENT.md`).

## Open issues

0. **VGA output works** (5 Nov 2026): the code is in `fl2000_vga.c`, see
   `docs/DEVELOPMENT.md`, "VGA output". Tested with the LG 19EN43: the
   connector is `connected` by itself with nothing on HDMI; GNOME works at
   1024x768, 1280x720, 1280x768, 1280x800 and the panel's native 1366x768
   (issue 7); GTT stayed flat over 10 min (94 -> 86 MB); unplugging and
   replugging the VGA cable works without errors; and HDMI still works next
   to it (1080p, GNOME switches between the two without logging out).
   VGA-only adapters without an IT66121 are not supported.
1. **Hot replug.** Unplug and replug are clean in the kernel, the colour-bar
   test works afterwards (`docs/field-test-15_04.log`). Under GNOME the result
   depends on the DRM node number: with Mutter 51.1 a replug that gets a new
   node (card2 -> card3) shows the monitor, one that reuses the old node
   (`device already present`) does not and also drops the other monitor until
   the next login. Mutter 50.4 never added the output (and once crashed). Not
   fixable from the driver; findings and what not to retry are in
   `docs/DEVELOPMENT.md`, "Unplug and replug". A short desktop stall while the
   dongle connects was traced to re-reading the EDID on every probe; the fix
   (keep the EDID until HPD drops) is in and the log shows one read per
   replug, but the stall itself has not been re-checked. The full field
   test on a clean boot is still to be repeated on the current branch. Quick
   root-free check after a test boot: `scripts/post-boot-check.sh`.
2. **Frame pacing.** Vblank now comes from the DRM vblank timer at the mode's
   refresh rate (it used to follow every URB completion). The stream itself
   resends the last frame as soon as a buffer completes, but measured on 5 Nov
   the device already paces it by USB flow control: `sent/s` equals the
   refresh rate at every mode it displays, and an unchanged frame is resent
   without any copy. Little seems left to gain; re-measure before working on it.
3. The IT66121 EDID FIFO path never succeeds on this hardware (harmless: it
   costs ~500 ms of timeouts before the direct path runs, and it is kept because
   other board wirings may need it).
4. `sysfs` `edid` file has read empty while the connector property blob was
   populated. Unexplained, cosmetic so far.
5. **1920x1080@60.00 on HDMI.** HDMI only offers modes whose pixel clock the
   PLL hits within 500 ppm, which is why 59.94 Hz is used. Accepting 1122 ppm
   would program the very same registers as the verified mode (148.333 MHz) and
   make the 60.00 Hz mode valid: see `docs/pending/htotal-hdmi.md`, to try with
   an HDMI monitor at hand.
6. **USB bandwidth check is a no-op**: `fl2000_get_bytes_pix()` gets the
   pixel clock in kHz but treats it as Hz (and the 80 % margin is inverted),
   so RGB888 is always chosen. Harmless for 1080p60 on USB 3.0, wrong for
   USB 2.0 and for modes above ~166 MHz. Details in `docs/DEVELOPMENT.md`;
   needs hardware to decide the right behavior on USB 2.0.
7. **1366x768 on VGA: frame-end fix verified 5 Nov (`field-test-19_03.log`). GNOME showing only the bottom ~10 % at 1360/1366 was the amdgpu vmap of split VRAM; import fix ea3ea23 verified the same evening: GNOME at 1366x768 whole and correct, still zero-copy (AR30 imported, pitch 5632), 60 sent/s, 8.7 ms per full frame, GTT 94 MB.** HDMI 1080p also checked the same evening (59 sent/s, GNOME switched VGA<->HDMI without logging out).
   The frames had no end (see the zero-length URB invariant; commit 627c569),
   and the conversion at that width fell off the streaming-load path (78 ms
   per frame; commit b22b48e). To verify, from a real TTY:
   `sudo FL2000_TEST_MODE='1366x768 #14 1360x768 848x480 800x600 1024x768' bash scripts/fl2000-field-test.sh`
   (the `setting mode` line shows which mode each `#N` picked; `sent/s` must
   equal each mode's refresh rate), then GNOME at 1366x768 (`conv` ~8 ms per
   full frame). **1360x768 (mostly garbled) is a separate problem**: its frame
   is a whole number of packets and the device ran at one frame per refresh.
   Suspect the monitor's mode detection first; 848x480 tells a chip limit from
   the monitor. Details and rejected theories: `docs/DEVELOPMENT.md`, "VGA
   output".
8. **Upstream review items still open** (done: simple pipe removed, shadow
   plane, format helpers, vblank timer, fbdev, atomic bridge enable/disable,
   and on 5 Nov `drm_bridge_connector` + `DRM_BRIDGE_ATTACH_NO_CONNECTOR`
   with the mode programmed in `atomic_enable`, NOT yet tested on hardware:
   check HDMI with and without EDID, HDMI plug/unplug, VGA, and that
   `HDMI-A-n` keeps its name):
   - reuse the in-tree `ite-it66121` bridge (asked in the RFC cover letter)
   - no component framework (`drm_bridge_attach()` from probe), a single
     interface plus `usb_driver_claim_interface()`, DRM device parented to the
     USB interface with `drm_dev_set_dma_dev()`. This is the riskiest rework:
     it replaces the lifetime model behind the crash fixes in "Non-obvious
     invariants".
   - real suspend/resume (`drm_mode_config_helper_suspend()`),
     `DRM_BRIDGE_OP_HDMI` infoframes (the AVI infoframe is still written by
     hand), and maybe `DRM_BRIDGE_OP_HPD` (question 2 of the cover letter).
     Arch 7.2.11 has no `DRM_ITE_IT66121` either.
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
