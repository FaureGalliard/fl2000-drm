# Changelog

History of the changes made to the driver since it was forked from
[klogg/fl2000_drm](https://github.com/klogg/fl2000_drm). For the reasoning
behind each fix (symptoms, root causes, rejected theories) see
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Port to kernel 7.x (2026)

### Refcounted DRM bridges (mandatory since kernel 6.16 / 7.x)

- The IT66121 bridge is now allocated with `devm_drm_bridge_alloc()` instead of
  `kzalloc()`; its lifetime is managed by the DRM core reference counting.
- Removed the manual `kfree()` of the bridge private structure (freed by the
  bridge release once the last reference is dropped).

### Fixed kernel Oops on rebind after USB reset

- The IT66121 component was destroyed and re-created on every unbind/rebind
  cycle, which leaves dangling component pointers in the aggregate match data
  (`free_aggregate_device()` clears `c->adev` but not
  `match->compare[i].component`, and `component_del()` on an unbound component
  skips `remove_component()`), crashing in `component_bind_all()` on the next
  bind. The component now persists for the lifetime of the USB device (created
  once, with retry on cold-boot detection failures; destroyed when the device
  is unplugged).
- The IT66121 polling work and log messages no longer dereference
  `bridge->dev`, which is NULL while the bridge is detached between bind cycles.

### Fixed DRM device teardown for hot-unplug

- Fixed a kernel deadlock on disconnect: `it66121_destroy()` calls
  `component_del()`, which takes the global component mutex — but it was
  invoked from the master unbind callback, which already runs with that mutex
  held (`component_master_del()` → unbind). It is now called from
  `fl2000_disconnect()` after `component_master_del()` returns.
- `drm_dev_unplug()` + `drm_atomic_helper_shutdown()` now run on unbind, before
  components are detached (same pattern as in-tree USB display drivers such as
  gm12u320).
- Removed a double `drm_mode_config_cleanup()` (already managed by
  `drmm_mode_config_init()`) and a double `drm_dev_put()` (already managed by
  `devm_drm_dev_alloc()`), both of which caused use-after-free on disconnect.
- Fixed unbind devres pairing so the teardown callback actually runs.

### Fixed system-wide stalls while streaming (interrupts disabled too long)

- `fl2000_stream_compress()` (full-frame pixel conversion) and the resend
  `memcpy()` in `fl2000_stream_work()` ran while holding `list_lock`, which is
  taken with `spin_lock_irq()`. Each is several MB of work per frame, so
  interrupts were disabled for milliseconds at a time, continuously — the AMD
  GPU logged `Fence fallback timer expired` and the whole desktop turned laggy.
  The heavy data operations now run with the buffer owned but the lock
  released; only the list pointer updates stay under the lock.
- Fixed two `spin_unlock()` calls that were paired with `spin_lock_irq()` (in
  the compress and disable paths), which left interrupts disabled on return.
- Enabled the plane's `FB_DAMAGE_CLIPS` property that the update path already
  relies on (silences a DRM warning).

### Switched framebuffers from GEM DMA (CMA) to GEM SHMEM helpers

- The DMA helpers allocate physically contiguous framebuffers
  (`dma_alloc_wc`), and a 1080p buffer (~8.3 MB) exceeds the kernel's maximum
  contiguous allocation on x86 without CMA — every dumb buffer creation failed
  with `Cannot allocate memory`, so no compositor or `modetest` could light up
  the display.
- The FL2000 never DMAs the framebuffer: `fl2000_stream_compress()` reads it
  with the CPU and repacks it into the driver's own URB buffers, so paged shmem
  memory (as used by the in-tree USB display drivers udl, gm12u320, gud) is the
  correct choice; the dirty path now vmaps the framebuffer with
  `drm_gem_fb_vmap()`.

### EDID / DDC fixes

- Restored EDID reading through the IT66121 DDC master (EDID FIFO), now via
  `drm_edid_read_custom()`. The 6.x modernization had replaced it with
  `drm_get_edid()` on the FL2000 I²C bus, which hit the adapter's 1-byte-read
  quirks (`msg too long (addr 0x0050)`), so only fallback modes (1024x768 etc.)
  were ever offered.
- Added a direct-DDC EDID fallback: on Fresco reference designs the monitor's
  DDC lines hang directly off the FL2000 I²C bus, and the vendor driver reads
  EDID at address 0x50 with the FL2000's own dword I²C engine. If the IT66121
  EDID FIFO path fails, the driver retries that way.
- `it66121_wait_ddc_ready()` now actually polls the DDC done flag (the poll
  condition was hardcoded to `true`), tolerates bus contention (`wait bus` /
  `arbitration lose` are transients on a shared bus) and polls sparsely.
- DDC engine access is serialized with a mutex: the interrupt polling work could
  issue a DDC abort in the middle of an ongoing EDID read.
- Restored the HDCP release sequence in the DDC abort path (clear CP_DESIRE +
  HDCP engine reset + host master re-select, as in the in-tree ite-it66121
  driver). The HDCP engine held the DDC bus after reset (status `0x1A`) and
  every EDID read timed out.
- EDID/DDC failures are logged with the failing step and raw DDC status, and
  HPD state changes are logged, so field debugging no longer needs dynamic
  debug flags.
- EDID reads no longer dereference a NULL I²C adapter.
- Interrupt polling work is initialized once at bridge creation, so
  `cancel_delayed_work_sync()` can no longer touch an uninitialized work.

### Build system and tooling

- Replaced deprecated `EXTRA_CFLAGS` with `ccflags-y`.
- Added `scripts/fl2000-field-test.sh` to capture driver state, EDID, a modeset
  test and dmesg into a log file.

## Modernization for kernel 6.x

- Removed deprecated headers (`drm_fbdev_generic.h`, `drm_crtc_helper.h`,
  `drm_ioctl.h`) and added the modern ones (`drm_device.h`, `drm_client.h`,
  `drm_damage_helper.h`).
- `lastclose` → `postclose` in `drm_driver`; `it66121_bridge_attach()` takes
  the encoder parameter; `drm_helper_hpd_irq_event()` →
  `drm_bridge_hpd_notify()`.
- Added `install.sh` with automatic Secure Boot module signing.
- Removed `drm_kms_helper_poll_init/fini` and `drm_fbdev_generic_setup`.
