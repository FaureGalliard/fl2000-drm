# FL2000 USB Display Driver for Linux Kernel 7.x

<p align="center">
  <img src="https://img.shields.io/badge/License-GPL%20v2-blue.svg" alt="License">
  <img src="https://img.shields.io/badge/Kernel-6.16%2B%20%2F%207.x-green.svg" alt="Kernel Version">
  <img src="https://img.shields.io/badge/Architecture-x86__64-orange.svg" alt="Architecture">
</p>

## Overview

This is a modernized version of the FL2000 DRM driver, updated to work with Linux kernel 7.x (also builds on kernel 6.16 and later). The driver supports USB-to-HDMI adapters based on the Fresco Logic FL2000DX chip with the IT66121FN HDMI bridge.

## Features

- **Modern DRM API**: Updated to use current kernel 6.x DRM APIs
- **Atomic Modesetting**: Full support for atomic display configuration
- **Automatic Resolution**: Dynamically adjusts based on USB bandwidth
- **Secure Boot Ready**: Built-in module signing support
- **Plug & Play**: Automatic device detection
- **Triple Buffering**: Smooth rendering with multiple buffers

## Hardware Compatibility

| Device | Chipset | Max Resolution |
|--------|---------|----------------|
| USB HDMI Dongle | FL2000DX + IT66121FN | 4K@30Hz (USB 3.0), 1080p@60Hz |

- **USB 2.0**: Up to 1080p@30Hz
- **USB 3.0**: Up to 4K@30Hz or 1080p@60Hz

## Requirements

```bash
# Ubuntu/Debian
sudo apt-get update
sudo apt-get install build-essential linux-headers-$(uname -r)

# Verify kernel version (must be 6.16 or later, including 7.x)
uname -r
```

## Installation

### Quick Install (Recommended)

```bash
cd /home/samuel/driver/fl2000_drm
sudo ./install.sh
```

The installation script will:
1. Compile the driver for your kernel
2. Detect Secure Boot status
3. Sign modules if required
4. Install modules to the system
5. Load the driver

### Manual Installation

```bash
# Compile
make clean
make KVER=$(uname -r)

# Install (as root)
sudo cp fl2000.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a

# Load
sudo modprobe fl2000
```

### Secure Boot Setup

If Secure Boot is enabled, the driver needs to be signed:

```bash
# Generate signing keys (one-time setup)
sudo openssl req -new -x509 -newkey rsa:2048 \
  -keyout /var/lib/dkms/mok.key \
  -out /var/lib/dkms/mok.pub \
  -days 3650 -nodes \
  -subj "/CN=FL2000 Driver/"

# Sign modules
sudo /usr/bin/sign-file sha256 /var/lib/dkms/mok.key \
  /var/lib/dkms/mok.pub /lib/modules/$(uname -r)/extra/fl2000.ko

# Import key for next boot
sudo mokutil --import /var/lib/dkms/mok.pub
```

## Verification

```bash
# Check module loaded
lsmod | grep fl2000

# Check driver messages
dmesg | grep -i fl2000

# List DRM devices
ls -la /dev/dri/

# Get device info
cat /sys/class/drm/*/device/name
```

## Project Structure

```
fl2000_drm/
├── drivers/gpu/drm/fl2000/     # Driver, laid out as in the kernel tree
│   ├── Kconfig / Makefile      # Kbuild integration (CONFIG_DRM_FL2000)
│   ├── fl2000.h                # Main header file
│   ├── fl2000_drv.c            # USB driver core
│   ├── fl2000_drm.c            # DRM implementation
│   ├── fl2000_streaming.c      # USB streaming
│   ├── fl2000_i2c.c            # I2C interface
│   ├── fl2000_interrupt.c      # Interrupt handling
│   ├── fl2000_registers.[ch]   # Hardware registers
│   └── fl2000_it66121.[ch]     # IT66121 HDMI bridge
├── Makefile                    # Out-of-tree build wrapper
├── install.sh                  # Installation script
└── README.md                   # This file
```

## Troubleshooting

### The usb-storage quirk shows empty after reboot

`usb_storage` is loaded from the initramfs during early boot, and the
initramfs bundles its own copy of `/etc/modprobe.d`. If the initramfs was
generated before creating `fl2000-storage.conf`, the quirk silently does not
apply. Regenerate it once:

```bash
sudo mkinitcpio -P
```

`scripts/fl2000-field-test.sh` also force-applies the quirk at runtime and
unbinds usb-storage from the FL2000's virtual CD if it already claimed it.

### Adapter disconnects ~30 seconds after plugging in

These dongles expose a virtual CD-ROM (mass storage, `/dev/sr0`) with Windows
drivers alongside the display interfaces. When the display driver initializes
the chip, the storage function can stop responding; the SCSI layer then times
out (30 s) and issues a USB reset of the whole device, which unbinds the
display driver. Tell the kernel to ignore the storage function:

```bash
# Make usb-storage ignore the FL2000 virtual CD-ROM (takes effect on next plug)
echo 'options usb-storage quirks=1d5c:2000:i' | sudo tee /etc/modprobe.d/fl2000-storage.conf
sudo modprobe -r usb_storage 2>/dev/null; sudo modprobe usb_storage

# Or apply immediately without reloading modules:
echo '1d5c:2000:i' | sudo tee /sys/module/usb_storage/parameters/quirks
```

The virtual CD only contains Windows/Mac drivers, so nothing is lost.

### Driver won't load

1. Check kernel headers installed:
   ```bash
   ls /lib/modules/$(uname -r)/build
   ```

2. Check error messages:
   ```bash
   dmesg | tail -50
   ```

3. Verify USB device connected:
   ```bash
   lsusb | grep -i fresco
   ```

### No display output

1. Check EDID reading:
   ```bash
   dmesg | grep -i edid
   ```

2. Verify monitor detection:
   ```bash
   cat /sys/class/drm/card1-*/status
   ```

3. Try manual mode setting:
   ```bash
   xrandr --output DP-1 --auto
   ```

## Known Issues

- Audio over HDMI not implemented (video only)
- Multiple adapters untested
- Limited to single display per adapter

## License

GPL v2 - See [LICENSE](./LICENSE) file for details.

## Contributors

### Original Project
- **Marcus Comstedt** (@zeldin) - Original driver development
- **Marcel Waldvogel** (@MarcelWaldvogel) - Driver improvements
- **Artem Mygaiev** - Original DRM driver implementation and maintainer

### Kernel 6.x Modernization (2026)
- **Samuel** - Used original code and updated for Linux Kernel 6.x

### What Was Done

#### Port to Kernel 7.x (July 2026):
1. **Refcounted DRM bridges** (mandatory since kernel 6.16/7.x)
   - IT66121 bridge is now allocated with `devm_drm_bridge_alloc()` instead of
     `kzalloc()`; its lifetime is managed by the DRM core reference counting
   - Removed manual `kfree()` of the bridge private structure (freed by the
     bridge release once the last reference is dropped)
2. **Fixed kernel Oops on rebind after USB reset**
   - The IT66121 component was destroyed and re-created on every
     unbind/rebind cycle, which leaves dangling component pointers in the
     aggregate match data (`free_aggregate_device()` clears `c->adev` but not
     `match->compare[i].component`, and `component_del()` on an unbound
     component skips `remove_component()`), crashing in
     `component_bind_all()` on the next bind. The component now persists for
     the lifetime of the USB device (created once, with retry on cold-boot
     detection failures; destroyed via devres when the device is unplugged)
   - The IT66121 polling work and log messages no longer dereference
     `bridge->dev`, which is NULL while the bridge is detached between bind
     cycles
3. **Fixed DRM device teardown for hot-unplug**
   - Fixed kernel deadlock on disconnect: `it66121_destroy()` calls
     `component_del()`, which takes the global component mutex — but it was
     invoked from the master unbind callback, which already runs with that
     mutex held (`component_master_del()` → unbind). It is now called from
     `fl2000_disconnect()` after `component_master_del()` returns
   - `drm_dev_unplug()` + `drm_atomic_helper_shutdown()` now run on unbind,
     before components are detached (same pattern as in-tree USB display
     drivers such as gm12u320)
   - Removed double `drm_mode_config_cleanup()` (already managed by
     `drmm_mode_config_init()`) and a double `drm_dev_put()` (already managed
     by `devm_drm_dev_alloc()`), both of which caused use-after-free on
     disconnect
   - Fixed unbind devres pairing so the teardown callback actually runs
4. **Fixed system-wide stalls while streaming (interrupts disabled too long)**
   - `fl2000_stream_compress()` (full-frame pixel conversion) and the resend
     `memcpy()` in `fl2000_stream_work()` ran while holding `list_lock`, which
     is taken with `spin_lock_irq()` (interrupts disabled). Each is several MB
     of work per frame, so interrupts were disabled for milliseconds at a
     time, continuously — starving the rest of the system. The AMD GPU logged
     `Fence fallback timer expired` repeatedly and the whole desktop (both the
     laptop panel and the USB display) turned laggy. The heavy data operations
     now run with the buffer owned but the lock released; only the list
     pointer updates stay under the lock
   - Fixed two `spin_unlock()` calls that were paired with `spin_lock_irq()`
     (in the compress and disable paths), which left interrupts disabled on
     return
   - Enabled the plane's `FB_DAMAGE_CLIPS` property that the update path
     already relies on (silences a DRM warning)
5. **Switched framebuffers from GEM DMA (CMA) to GEM SHMEM helpers**
   - The DMA helpers allocate physically contiguous framebuffers
     (`dma_alloc_wc`), and a 1080p buffer (~8.3 MB) exceeds the kernel's
     maximum contiguous allocation on x86 without CMA — every dumb buffer
     creation failed with `Cannot allocate memory`, so no compositor or
     `modetest` could ever light up the display
   - The FL2000 never DMAs the framebuffer: `fl2000_stream_compress()` reads
     it with the CPU and repacks it into the driver's own URB buffers, so
     paged shmem memory (as used by the in-tree USB display drivers udl,
     gm12u320, gud) is the correct choice; the dirty path now vmaps the
     framebuffer with `drm_gem_fb_vmap()`
6. **Fixes found during the port**
   - Restored EDID reading through the IT66121 DDC master (EDID FIFO), now
     via `drm_edid_read_custom()`. The 6.x modernization had replaced it with
     `drm_get_edid()` on the FL2000 I2C bus, which cannot work: the monitor's
     DDC lines are wired to the IT66121, and the FL2000 bus quirks only allow
     1-byte reads (dmesg showed `adapter quirk: msg too long (addr 0x0050)`),
     so only fallback modes (1024x768 etc.) were ever offered and desktop
     compositors ignored the output
   - `it66121_wait_ddc_ready()` now actually polls the DDC done flag (the
     poll condition was hardcoded to `true`)
   - DDC engine access is serialized with a mutex: the interrupt polling work
     could issue a DDC abort in the middle of an ongoing EDID read (the
     original driver's `XXX: lock` placeholders, now implemented)
   - Restored the HDCP release sequence in the DDC abort path (clear
     CP_DESIRE + HDCP engine reset + host master re-select, as in the in-tree
     ite-it66121 driver). The HDCP engine shares the DDC master and held the
     bus after reset — DDC status reported `wait bus` + `arbitration lose`
     (0x1A) and every EDID read timed out. The original driver had removed
     this sequence as "HDCP is not supported"
   - Made the DDC completion wait tolerate bus contention: on this hardware
     the FL2000 I2C master and the IT66121 DDC master share the physical bus,
     so `wait bus`/`arbitration lose` are live/latched transients of the
     engine retrying (status `0x4A` = active + arbi-lose, `0x1A` while backing
     off). The wait now polls only for TX_DONE with a generous timeout, and
     polls sparsely to reduce self-inflicted contention
   - Added a direct-DDC EDID fallback: on Fresco reference designs the
     monitor's DDC lines hang directly off the FL2000 I2C bus and the vendor
     driver reads EDID at address 0x50 with the FL2000's own dword I2C engine.
     If the IT66121 EDID FIFO path fails, the driver now retries reading the
     EDID that way, bypassing the IT66121 DDC master
   - EDID/DDC failures are logged with the failing step and raw DDC status,
     and HPD state changes are logged, so field debugging no longer needs
     dynamic debug flags
   - EDID reads no longer dereference a NULL I2C adapter
     (`priv->adapter` was never assigned)
   - Interrupt polling work is initialized once at bridge creation, so
     `cancel_delayed_work_sync()` can no longer touch an uninitialized work
7. **Build system**
   - Replaced deprecated `EXTRA_CFLAGS` with `ccflags-y`

#### Modernization for Kernel 6.x:
1. **Updated DRM Headers**
   - Removed deprecated headers (`drm_fbdev_generic.h`, `drm_crtc_helper.h`, `drm_ioctl.h`)
   - Added modern DRM headers (`drm_device.h`, `drm_client.h`, `drm_damage_helper.h`)

2. **Fixed Deprecated API Calls**
   - Changed `lastclose` → `postclose` in `drm_driver`
   - Updated `it66121_bridge_attach()` signature to include encoder parameter
   - Changed `drm_helper_hpd_irq_event()` → `drm_bridge_hpd_notify()`
   - Replaced `drm_do_get_edid()` with `drm_get_edid()`

3. **Added Secure Boot Support**
   - Created `install.sh` script with automatic module signing
   - MOK key generation and enrollment support

4. **Documentation**
   - Improved README.md with installation instructions
   - Created comprehensive GitHub Wiki with technical details
   - Added troubleshooting guides

#### Removed/Disabled Features:
- `drm_kms_helper_poll_init/fini` - Not needed in modern kernels
- `drm_plane_enable_fb_damage_clips` - Not compatible
- `drm_fbdev_generic_setup` - Removed (fbdev emulation is deprecated)
- `drm_atomic_helper_damage_merged` - Compatibility issues

## Links

- [Development notes](./docs/DEVELOPMENT.md) - Hardware map, DDC status decoding and bug history
- [GitHub Wiki](https://github.com/Samuv5/fl2000_drm/wiki) - Detailed technical documentation
- [Original Repository](https://github.com/klogg/fl2000_drm)
- [Linux DRM Documentation](https://www.kernel.org/doc/html/latest/gpu/drm-kms-helpers.html)

---

*For detailed technical documentation, see [WIKI.md](./WIKI.md)*