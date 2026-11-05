#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Read and write FL2000 registers from userspace, for experiments with the
# hardware while the kernel driver stays loaded. It uses the same vendor
# control transfers as the driver (request 0x40 reads, 0x41 writes, wIndex is
# the register, 32-bit little endian data) through the usbfs ioctl, so it needs
# root and no extra packages.
#
#   sudo python3 scripts/fl2000-reg.py status          decoded key registers
#   sudo python3 scripts/fl2000-reg.py watch           print status changes
#   sudo python3 scripts/fl2000-reg.py edid [file]     read the monitor EDID at 0x50
#   sudo python3 scripts/fl2000-reg.py edid-wait [file]  retry for 60 s while you replug the cable
#   sudo python3 scripts/fl2000-reg.py read 0x803c
#   sudo python3 scripts/fl2000-reg.py write 0x803c 0x0
#   sudo python3 scripts/fl2000-reg.py set 0x802c 26   set bit 26
#   sudo python3 scripts/fl2000-reg.py clear 0x802c 26
#
# The 'edid' command drives the chip's I2C engine exactly like the kernel driver
# does, so it can collide with the driver's own use of it (the IT66121 polling at
# 0x4c): if the checksum is wrong, run it again.
#
# Caveats: reading 0x8000 clears its 'read self clear' bits, so the driver can
# miss an event while this runs; the driver caches non-volatile registers, so it
# may later write back a stale value over what is set here. Use it for
# experiments, not in normal operation.

import ctypes
import fcntl
import glob
import os
import struct
import sys
import time

VID, PID = 0x1D5C, 0x2000
REQ_READ, REQ_WRITE = 0x40, 0x41

# struct usbdevfs_ctrltransfer { u8 type; u8 req; u16 value; u16 index; u16 len; u32 timeout; void *data; }
class CtrlTransfer(ctypes.Structure):
    _fields_ = [("bRequestType", ctypes.c_uint8), ("bRequest", ctypes.c_uint8),
                ("wValue", ctypes.c_uint16), ("wIndex", ctypes.c_uint16),
                ("wLength", ctypes.c_uint16), ("timeout", ctypes.c_uint32),
                ("data", ctypes.c_void_p)]


USBDEVFS_CONTROL = (3 << 30) | (ctypes.sizeof(CtrlTransfer) << 16) | (ord('U') << 8) | 0

# name -> (offset, [(bit or (hi, lo), name), ...]) taken from fl2000_registers.h
REGS = {
    "STATUS": (0x8000, [(0, "VGA_STATUS"), (1, "VGA_ERROR"), (2, "LBUF_HALT"), (3, "ISO_ACK"),
                        (4, "TD_DROP"), (5, "IRQ_PENDING"), (6, "PLL_STATUS"),
                        (7, "DAC_STATUS"), (8, "LBUF_OVERFLOW"), (9, "LBUF_UNDERFLOW"),
                        ((25, 10), "FRAME_CNT"), (26, "HDMI_EVENT"), (27, "HDMI_STATUS"),
                        (28, "EDID_STATUS"), (29, "MONITOR_STATUS"), (30, "MONITOR_EVENT"),
                        (31, "EDID_EVENT")]),
    "PXCLK": (0x8004, [(0, "CLEAR_WATERMARK"), (1, "FRAME_SYNC"), (2, "HSYNC_POLARITY"),
                       (3, "VSYNC_POLARITY"), (4, "DE_POLARITY"), (5, "MIRROR_MODE"),
                       (6, "VGA565_MODE"), (7, "DAC_OUTPUT_EN"), (8, "VGA_TIMING_EN"),
                       (9, "USE_NEW_PKT_RETRY"), (10, "REF_SELECT"), (11, "DAC_PX_CLK_INVERT"),
                       (12, "CLEAR_LBUF_STATUS"), (13, "DROP_CNT"), (14, "USE_VDI_ITP_CNT"),
                       (24, "VGA_COMPRESS"), (25, "VGA332_MODE"), (26, "COLOR_PALETTE_EN"),
                       (27, "FIRST_BT_ENC_EN"), (28, "CLEAR_125US_CNT"), (29, "DISABLE_HALT"),
                       (30, "FORCE_DE_EN"), (31, "VGA555_MODE")]),
    "I2C_SC": (0x8020, [((6, 0), "ADDR"), (7, "CMD"), ((15, 8), "OFFSET"),
                        ((23, 16), "VGA_STATUS"), ((27, 24), "STATUS"),
                        (28, "MONITOR_DETECT"), (29, "READY"), (30, "EDID_DETECT"),
                        (31, "DONE")]),
    "PLL": (0x802C, [((7, 0), "DIVISOR"), ((9, 8), "PRESCALER"), ((14, 13), "FUNCTION"),
                     ((23, 16), "MULTIPLIER"), (24, "TEST_IO"), (25, "CFG_DAC_PWRDOWN"),
                     (26, "FORCE_DAC_PWRUP")]),
    "ACLK": (0x803C, [(0, "CFG_TIMING_RESET_N"), (1, "PLH_BLOCK_EN"), (2, "EDID_MON_INT_EN"),
                      (3, "EXT_MON_INT_EN"), (4, "VGA_STATUS_SELF_CLEAR"),
                      ((9, 5), "PLL_LOCK_TIME"), (10, "PLL_FAST_TIMEOUT_EN"),
                      (11, "PPE_BLOCK_EM"), (12, "PLL_TIMER_EN"), (13, "FEEDBACK_INT_EN"),
                      (14, "CLR_125US_COUNTER"), (15, "CCS_PD_DIS"), (16, "STANDBY_EN"),
                      (17, "FORCE_LOOPBACK"), (18, "LBUF_DROP_FRAME_EN"),
                      (19, "LBUF_VDE_RST_EN"), (20, "LBUF_SW_RST"), (21, "LBUF_ERR_INT_EN"),
                      (22, "BIAC_EN"), (23, "PXCLK_IN_EN"), (24, "VGA_ERR_INT_EN"),
                      (25, "FORCE_VGA_CONNECT"), (26, "FORCE_PLL_UP"), (27, "USE_ZERO_TD"),
                      (28, "USE_ZERO_PKT_LEN"), (29, "USE_PKT_PENDING"),
                      (30, "PLL_DAC_PD_USBP3_EN"), (31, "PLL_DAC_PD_NOVGA_EN")]),
    "CTRL2": (0x8064, [(0, "PLL_POWERDOWN_DETECT_EN"), ((3, 1), "MSTOR_BLKSIZE_PTR_WIDTH"),
                       ((15, 4), "MSTOR_BLK_COUNT"), (16, "SPI_WR_EN"),
                       (17, "DETECT_PINS_DEBOUNCE_EN"), (18, "HDMI_INT_EN"),
                       (19, "HDMI_INT_ACTIVE_HIGH"), (20, "SPI_EN"),
                       ((30, 23), "SW_PROD_REV")]),
}


def find_device():
    for d in glob.glob("/sys/bus/usb/devices/*"):
        try:
            vid = int(open(d + "/idVendor").read(), 16)
            pid = int(open(d + "/idProduct").read(), 16)
            bus = int(open(d + "/busnum").read())
            dev = int(open(d + "/devnum").read())
        except (OSError, ValueError):
            continue
        if (vid, pid) == (VID, PID):
            return "/dev/bus/usb/%03d/%03d" % (bus, dev)
    sys.exit("FL2000 (1d5c:2000) not found")


class Fl2000:
    def __init__(self):
        self.fd = os.open(find_device(), os.O_RDWR)

    def _ctrl(self, rtype, req, index, buf):
        c = CtrlTransfer(rtype, req, 0, index, len(buf), 1000,
                         ctypes.cast(buf, ctypes.c_void_p))
        ret = fcntl.ioctl(self.fd, USBDEVFS_CONTROL, c)
        if ret != len(buf):
            raise OSError("short control transfer (%d)" % ret)

    def read(self, reg):
        buf = ctypes.create_string_buffer(4)
        self._ctrl(0xC0, REQ_READ, reg, buf)
        return struct.unpack("<I", buf.raw)[0]

    def write(self, reg, val):
        buf = ctypes.create_string_buffer(struct.pack("<I", val & 0xFFFFFFFF), 4)
        self._ctrl(0x40, REQ_WRITE, reg, buf)


def decode(val, fields):
    out = []
    for pos, name in fields:
        if isinstance(pos, tuple):
            hi, lo = pos
            out.append("%s=%d" % (name, (val >> lo) & ((1 << (hi - lo + 1)) - 1)))
        elif (val >> pos) & 1:
            out.append(name)
    return " ".join(out)


I2C_SC, I2C_RD = 0x8020, 0x8024


def i2c_read_dword(dev, addr, offset):
    # Same sequence as fl2000_i2c_dword() in the driver (read case)
    mask = (1 << 28) | (1 << 30) | (0xF << 24) | 0x7F | (1 << 7) | (0xFF << 8) | (1 << 31)
    val = (1 << 28) | (1 << 30) | (addr & 0x7F) | (1 << 7) | ((offset & 0xFF) << 8)
    dev.write(I2C_SC, (dev.read(I2C_SC) & ~mask) | val)
    deadline = time.time() + 1.0
    while True:
        v = dev.read(I2C_SC)
        if v & (1 << 31):
            break
        if time.time() > deadline:
            raise OSError("I2C engine timeout")
        time.sleep(0.002)
    if (v >> 24) & 0xF:
        raise OSError("I2C error status %d at offset 0x%02x" % ((v >> 24) & 0xF, offset))
    return dev.read(I2C_RD)


def read_edid(dev):
    data = bytearray()
    for off in range(0, 128, 4):
        data += struct.pack("<I", i2c_read_dword(dev, 0x50, off))
    return bytes(data)


def parse_edid(b):
    print("EDID checksum:", "ok" if sum(b[:128]) & 0xFF == 0 else "WRONG (%d)" % (sum(b[:128]) & 0xFF))
    print("header:", "ok" if b[:8] == bytes([0, 255, 255, 255, 255, 255, 255, 0]) else "INVALID")
    mid = (b[8] << 8) | b[9]
    print("manufacturer %s, product 0x%04x, week %d year %d, EDID %d.%d, %s input, %d extension(s)" %
          ("".join(chr(((mid >> sh) & 0x1F) + 64) for sh in (10, 5, 0)), b[10] | (b[11] << 8),
           b[16], b[17] + 1990, b[18], b[19], "digital" if b[20] & 0x80 else "analog", b[126]))
    for i in range(4):
        d = b[54 + 18 * i:72 + 18 * i]
        if d[0] | d[1]:
            clk = (d[0] | (d[1] << 8)) * 10
            ha, hb = d[2] | ((d[4] & 0xF0) << 4), d[3] | ((d[4] & 0x0F) << 8)
            va, vb = d[5] | ((d[7] & 0xF0) << 4), d[6] | ((d[7] & 0x0F) << 8)
            rate = clk * 1000.0 / ((ha + hb) * (va + vb)) if (ha + hb) * (va + vb) else 0
            print("timing: %dx%d @ %.2f Hz, pixel clock %.2f MHz" % (ha, va, rate, clk / 1000.0))
        elif d[3] == 0xFC:
            print("name:", d[5:18].decode(errors="replace").split("\n")[0].strip())
        elif d[3] == 0xFD:
            print("range limits: V %d-%d Hz, H %d-%d kHz, max pixel clock %d MHz" %
                  (d[5], d[6], d[7], d[8], d[9] * 10))


def show(dev, name):
    off, fields = REGS[name]
    val = dev.read(off)
    print("%-7s 0x%04x = 0x%08x  %s" % (name, off, val, decode(val, fields)))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    dev = Fl2000()
    num = lambda s: int(s, 0)

    if cmd == "status":
        for name in ("STATUS", "PXCLK", "I2C_SC", "PLL", "ACLK", "CTRL2"):
            show(dev, name)
    elif cmd in ("edid", "edid-wait"):
        deadline = time.time() + (60 if cmd == "edid-wait" else 0)
        if cmd == "edid-wait":
            print("Waiting up to 60 s: unplug and replug the VGA cable now", flush=True)
        last = None
        while True:
            st = dev.read(REGS["STATUS"][0])
            try:
                data = read_edid(dev)
                break
            except OSError as e:
                if cmd == "edid" or time.time() > deadline:
                    sys.exit("no EDID: %s (MONITOR_STATUS=%d EDID_STATUS=%d)" %
                             (e, (st >> 29) & 1, (st >> 28) & 1))
                msg = "no EDID: %s (MONITOR_STATUS=%d EDID_STATUS=%d)" % (
                    e, (st >> 29) & 1, (st >> 28) & 1)
                if msg != last:
                    print(time.strftime("%H:%M:%S"), msg, flush=True)
                    last = msg
                time.sleep(0.2)
        print(time.strftime("%H:%M:%S"), "EDID read")
        print(" ".join("%02x" % x for x in data[:16]), "...")
        parse_edid(data)
        if len(sys.argv) > 2:
            open(sys.argv[2], "wb").write(data)
            print("saved to", sys.argv[2])
    elif cmd == "watch":
        # The frame counter changes all the time while the chip outputs frames: ignore it
        frame_cnt = ((1 << 16) - 1) << 10
        last = None
        while True:
            val = dev.read(REGS["STATUS"][0])
            if last is None or (val ^ last) & ~frame_cnt:
                print(time.strftime("%H:%M:%S"), "STATUS 0x%08x  %s" %
                      (val, decode(val, REGS["STATUS"][1])), flush=True)
                last = val
            time.sleep(0.25)
    elif cmd == "read":
        reg = num(sys.argv[2])
        print("0x%04x = 0x%08x" % (reg, dev.read(reg)))
    elif cmd == "write":
        dev.write(num(sys.argv[2]), num(sys.argv[3]))
    elif cmd in ("set", "clear"):
        reg, bit = num(sys.argv[2]), num(sys.argv[3])
        val = dev.read(reg)
        val = val | (1 << bit) if cmd == "set" else val & ~(1 << bit)
        dev.write(reg, val)
        print("0x%04x = 0x%08x" % (reg, dev.read(reg)))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
