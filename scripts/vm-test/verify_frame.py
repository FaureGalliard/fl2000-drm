#!/usr/bin/python3 -I
# SPDX-License-Identifier: GPL-2.0
# verify_frame.py <dump> <w> <h> <sx> <sy> <bytes_pix> [x1 y1 x2 y2]: is the frame the pattern
# fl2000-t drew? The optional rectangle, in framebuffer coordinates, has the second pattern.
import sys
d = open(sys.argv[1], 'rb').read()
w, h, sx, sy, bp = map(int, sys.argv[2:7])
rect = list(map(int, sys.argv[7:11])) if len(sys.argv) > 7 else None
want_len = (w * h * bp + 7) & ~7
if len(d) != want_len:
    print('length %d, expected %d' % (len(d), want_len)); sys.exit(1)
def pat(x, y):
    return ((x * 7 + y * 13) & 0xff) | (((x >> 3) & 0xff) << 8) | ((y & 0xff) << 16)
bad = 0
step = max(1, h // 64)
for y in sorted(set(range(0, h, step)) | (set(range(max(rect[1] - sy - 2, 0), min(rect[3] - sy + 2, h))) if rect else set())):
    for x in sorted(set(range(0, w, 7)) | (set(range(max(rect[0] - sx - 2, 0), min(rect[2] - sx + 2, w))) if rect else set())):
        fx, fy = sx + x, sy + y
        if rect and rect[0] <= fx < rect[2] and rect[1] <= fy < rect[3]:
            fx, fy = fx + 1000, fy + 1000
        p = pat(fx, fy)
        if bp == 3:
            off = (y * w + x) * 3
            got = (d[off ^ 4], d[(off + 1) ^ 4], d[(off + 2) ^ 4])
            exp = (p & 0xff, (p >> 8) & 0xff, (p >> 16) & 0xff)
        else:
            off = (y * w + x) ^ 2
            got = d[off * 2] | d[off * 2 + 1] << 8
            exp = ((p & 0xF80000) >> 8) | ((p & 0xFC00) >> 5) | ((p & 0xF8) >> 3)
        if got != exp:
            bad += 1
            if bad < 4:
                print('pixel %d,%d: got %s, expected %s' % (x, y, got, exp))
print('frame %dx%d from %d,%d: %s' % (w, h, sx, sy, 'OK' if not bad else '%d bad pixels' % bad))
sys.exit(1 if bad else 0)
