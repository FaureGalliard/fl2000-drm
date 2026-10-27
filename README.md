<div align="center">

# 🖥️ FL2000 DRM — USB to HDMI on modern Linux

**Bring cheap Fresco Logic FL2000 USB 3.0 → HDMI adapters back to life on Linux kernel 7.x.**

A native DRM/KMS driver: your compositor sees a real display, with EDID, atomic modesetting and hot-plug — no DisplayLink, no proprietary blobs.

![Kernel](https://img.shields.io/badge/kernel-6.16%2B%20%7C%207.x-2ea043?logo=linux&logoColor=white)
[![License: GPL v2](https://img.shields.io/badge/license-GPL%20v2-blue)](LICENSE)
![Chipset](https://img.shields.io/badge/chipset-FL2000DX%20%2B%20IT66121-orange)
![USB ID](https://img.shields.io/badge/USB-1d5c%3A2000-555)
![Status](https://img.shields.io/badge/status-working%20%C2%B7%201080p60-2ea043)
[![Stars](https://img.shields.io/github/stars/FaureGalliard/fl2000-drm?style=flat)](https://github.com/FaureGalliard/fl2000-drm/stargazers)

**[🚀 Quick start](#-quick-start)** · [Hardware](#-supported-hardware) · [Troubleshooting](#-troubleshooting) · [Changelog](CHANGELOG.md) · [Development notes](docs/DEVELOPMENT.md)

</div>

---

## ✨ Why this driver

Fresco Logic never shipped a mainline driver for the FL2000, and the community
drivers stopped building years ago. This fork ports
[klogg/fl2000_drm](https://github.com/klogg/fl2000_drm) to current kernels and
fixes the bugs that kept it from ever lighting up a monitor:

- **🐧 Builds on kernel 6.16 → 7.x** — refcounted DRM bridges, modern EDID API,
  GEM SHMEM framebuffers. No version guards, no hacks.
- **🖼️ Real DRM/KMS device** — atomic modesetting, damage clips and hot-plug
  detection. GNOME, KDE and any Wayland/X11 compositor treat it like a normal
  output.
- **📺 Reads your monitor's EDID** — native resolution instead of 1024x768
  fallback modes.
- **🔌 Survives unplug and USB resets** — fixed the deadlocks, use-after-frees
  and Oopses that took the whole kernel down on disconnect.
- **⚡ Doesn't freeze your desktop** — pixel conversion no longer runs with
  interrupts disabled, so the rest of the system stays smooth while streaming.
- **🔐 Secure Boot ready** — `install.sh` signs the module with a MOK key.

> **Status:** works end-to-end on real hardware — GNOME on Wayland renders the
> desktop at **1920x1080@60** on the external monitor. Open work is tracked in
> [Known limitations](#-known-limitations).

---

## 🔧 Supported hardware

| Adapter | Chipset | USB ID | Status |
|---|---|---|---|
| Generic USB 3.0 → HDMI dongles (e.g. UGREEN) | Fresco Logic FL2000DX + ITE IT66121FN | `1d5c:2000` | ✅ 1920x1080@60 tested |

Check whether yours is one of them:

```bash
lsusb -d 1d5c:2000
```

Tested on Arch Linux, kernel 7.1.x, HP Pavilion 15 (amdgpu), LG 22MP55 monitor.
Other boards with the same chips should work — reports are welcome in
[Issues](https://github.com/FaureGalliard/fl2000-drm/issues).

---

## 🚀 Quick start

### 1. Install build dependencies

```bash
sudo pacman -S --needed base-devel linux-headers          # Arch
```

```bash
sudo apt install build-essential linux-headers-$(uname -r) # Debian / Ubuntu
```

### 2. Build and install

```bash
git clone https://github.com/FaureGalliard/fl2000-drm.git && cd fl2000-drm && sudo ./install.sh
```

`install.sh` compiles the module, signs it if Secure Boot is enabled, installs
it to `/lib/modules/$(uname -r)/extra/` and loads it.

<details>
<summary>Manual install</summary>

```bash
make clean && make
sudo cp fl2000.ko /lib/modules/$(uname -r)/extra/ && sudo depmod -a
sudo modprobe fl2000
```

The IT66121 bridge driver is built **into** `fl2000.ko` — there is no separate
`it66121.ko`.

</details>

### 3. Stop the virtual CD-ROM from resetting the adapter (required)

These dongles also expose a fake CD drive with Windows drivers. Left alone, it
times out and the kernel resets the whole device ~30 s after plugging in.

```bash
echo 'options usb-storage quirks=1d5c:2000:i' | sudo tee /etc/modprobe.d/fl2000-storage.conf
```

```bash
sudo mkinitcpio -P    # Arch; on Debian/Ubuntu: sudo update-initramfs -u
```

### 4. Reboot with the adapter connected and the monitor on

```bash
dmesg | grep -i -e fl2000 -e it66121
```

```bash
cat /sys/class/drm/card*-HDMI-*/status
```

You should see `EDID read ok` in `dmesg` and `connected` for the HDMI
connector. Your desktop's display settings will now list the new monitor.

---

## 🧩 How it works

```
USB 3.0 ──▶ FL2000DX ──┬── I²C (over USB control transfers)
                       │        ├── IT66121 HDMI bridge  @ 0x4C
                       │        └── monitor DDC / EDID   @ 0x50
                       ├── bulk EP1 OUT ── pixel stream ──▶ IT66121 ──▶ HDMI
                       ├── interrupt EP3 ── hot-plug / status events
                       └── mass storage (virtual CD — ignored via quirk)
```

```
compositor → DRM/KMS (fl2000_drm.c) → pixel repack (fl2000_streaming.c) → USB bulk URBs → FL2000 → IT66121 → monitor
```

```
fl2000.h               shared definitions
fl2000_drv.c           USB probe / disconnect, component master
fl2000_drm.c           DRM device, display pipe, modesetting
fl2000_streaming.c     framebuffer → USB pixel stream
fl2000_i2c.c           I²C adapter over USB control transfers
fl2000_interrupt.c     interrupt endpoint, hot-plug events
fl2000_registers.[ch]  FL2000 register map (regmap)
bridge/it66121_drv.c   IT66121 HDMI bridge + EDID readers
scripts/               field test script
docs/                  development notes and field test logs
```

The full hardware map, the meaning of every DDC status code and the history of
each bug are in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

---

## 🩺 Troubleshooting

<details>
<summary><b>The adapter disconnects ~30 seconds after plugging in</b></summary>

That is the virtual CD-ROM: the storage function stops responding once the
display driver initializes the chip, the SCSI layer times out and resets the
whole USB device. Apply the `usb-storage` quirk from
[step 3](#3-stop-the-virtual-cd-rom-from-resetting-the-adapter-required). To
apply it immediately without rebooting:

```bash
echo '1d5c:2000:i' | sudo tee /sys/module/usb_storage/parameters/quirks
```

The virtual CD only contains Windows/Mac drivers, so nothing is lost.

</details>

<details>
<summary><b>The usb-storage quirk is empty after reboot</b></summary>

`usb_storage` loads from the initramfs, which bundles its own copy of
`/etc/modprobe.d`. Regenerate it after creating `fl2000-storage.conf`:

```bash
sudo mkinitcpio -P
```

</details>

<details>
<summary><b>Nothing happens after unplugging and replugging</b></summary>

Hot replug frequently fails at the USB level: only the dongle's internal USB2
hub re-enumerates and the FL2000's SuperSpeed function never returns to the
bus. Leave it unplugged ~10 s before replugging, and check that it is back:

```bash
lsusb -d 1d5c:2000
```

Booting with the adapter already connected is the most reliable path.

</details>

<details>
<summary><b>Only 1024x768 or no modes at all</b></summary>

The driver could not read the monitor's EDID. Check the log:

```bash
dmesg | grep -i -e edid -e ddc
```

`DDC not done (-110)` followed by `EDID read ok` is **normal** on this
hardware (the IT66121 FIFO path fails, the direct path succeeds). See
[Decoding the driver's DDC messages](docs/DEVELOPMENT.md#decoding-the-drivers-ddc-messages).

</details>

<details>
<summary><b>Collecting a full report</b></summary>

Run the field test from a TTY (Ctrl+Alt+F3) and attach the log to an issue:

```bash
sudo bash scripts/fl2000-field-test.sh
```

It captures module/USB/DRM state, connectors, modes, EDID, a modeset color-bar
test and `dmesg` into `docs/field-test-HH_MM.log`.

</details>

---

## 🚧 Known limitations

- Video only — HDMI audio is not implemented.
- Frame pacing follows the USB transfer rate instead of the mode's refresh rate.
- Hot replug depends on the dongle's SuperSpeed function re-enumerating (see
  [Troubleshooting](#-troubleshooting)).
- Multiple adapters at once are untested.

---

## 🤝 Contributing

Bug reports with a field test log are the most useful contribution — every fix
in this driver came from a real log. Read [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)
before changing anything around locking, component lifetime or EDID; those
areas have caused real crashes. Commits follow the
[Conventional Commits](https://www.conventionalcommits.org/) style.

---

## 📄 License

[GPL v2](LICENSE), like the Linux kernel.

## 👏 Credits

- **Artem Mygaiev** ([klogg/fl2000_drm](https://github.com/klogg/fl2000_drm)) — original DRM driver and maintainer
- **Marcus Comstedt** (@zeldin) and **Marcel Waldvogel** (@MarcelWaldvogel) — driver improvements
- **Samuel** ([Samuv5/fl2000_drm](https://github.com/Samuv5/fl2000_drm)) — modernization for kernel 6.x
- **Angel Gabriel Crispin Valdivia** ([@FaureGalliard](https://github.com/FaureGalliard)) — port to kernel 7.x and hardware bring-up
