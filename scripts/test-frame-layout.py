#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Userspace test of the frame layout code in fl2000_streaming.c.
#
# The FL2000 wants the two 32-bit halves of every 64-bit word of the frame
# swapped, and the frame is one flat stream, so a line of a width whose byte
# length is not a multiple of 8 (1366 pixels at 3 bytes) starts in the middle of
# a word. This script extracts the real converters and the
# rectangle copy from the driver source, builds them against a few stubs and
# compares them, for many widths and rectangles, with an independent reference:
# convert the frame linearly, then swap the halves of each 64-bit word. The damage
# conversion is also run with framebuffers larger than the frame they show.
#
#   python3 scripts/test-frame-layout.py [--mutate]
#
# --mutate breaks the code on purpose (drops the word rounding of the rectangle
# copy) to show that the test notices.

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, 'drivers/gpu/drm/fl2000/fl2000_streaming.c')

FUNCS = ['fl2000_copy_rect',
         'fl2000_xrgb8888_to_rgb888_line', 'fl2000_xrgb8888_to_rgb565_line',
         'fl2000_xrgb2101010_to_xrgb8888_line', 'fl2000_read_span', 'fl2000_stream_compress']

PRELUDE = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define clamp(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define round_down(x, y) ((x) & ~((__typeof__(x))(y) - 1))
#define round_up(x, y) ((((x) - 1) | ((__typeof__(x))(y) - 1)) + 1)
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((__typeof__(x))(a) - 1)) == 0)
#define guard(t) (void)
#define FL2000_DAMAGE_LOG 16
#define DRM_FORMAT_XRGB8888 1
#define DRM_FORMAT_XRGB2101010 2
struct drm_rect { int x1, y1, x2, y2; };
static int drm_rect_width(const struct drm_rect *r) { return r->x2 - r->x1; }
static int drm_rect_height(const struct drm_rect *r) { return r->y2 - r->y1; }
static bool drm_rect_intersect(struct drm_rect *r, const struct drm_rect *c)
{
	r->x1 = max(r->x1, c->x1);
	r->y1 = max(r->y1, c->y1);
	r->x2 = min(r->x2, c->x2);
	r->y2 = min(r->y2, c->y2);
	return r->x1 < r->x2 && r->y1 < r->y2;
}
struct iosys_map { union { void *vaddr; void *vaddr_iomem; }; bool is_iomem; };
#define IOSYS_MAP_INIT_VADDR(v) { .vaddr = (v), .is_iomem = false }
static void iosys_map_incr(struct iosys_map *m, size_t n) { m->vaddr = (u8 *)m->vaddr + n; }
static void iosys_map_memcpy_from(void *d, const struct iosys_map *m, size_t off, size_t n)
{
	memcpy(d, (const u8 *)m->vaddr + off, n);
}
static void drm_memcpy_from_wc(struct iosys_map *d, const struct iosys_map *s, unsigned long n)
{
	memcpy(d->vaddr, s->vaddr, n);
}
struct fl2000_stream {
	u32 width, height, bytes_pix;
	size_t buf_size;
	void *shadow;
	int shadow_lock;
	u32 shadow_seq;
	struct drm_rect damage_log[FL2000_DAMAGE_LOG];
	u32 *line_buf;
};
'''

MAIN = r'''
static uint64_t rs = 88172645463325252ull;
static u32 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (u32)(rs >> 11); }
static u32 rndn(u32 n) { return n ? rnd() % n : 0; }

static size_t frame_size(u32 w, u32 h, u32 bpp) { return ((size_t)w * h * bpp + 7) & ~(size_t)7; }

static u16 pack565(u32 pix)
{
	return ((pix & 0x00F80000) >> 8) | ((pix & 0x0000FC00) >> 5) | ((pix & 0x000000F8) >> 3);
}

/* Reference: linear frame, then swap the 32-bit halves of every 64-bit word */
static u8 *ref_convert(const u32 *src, u32 w, u32 h, u32 bpp)
{
	size_t sz = frame_size(w, h, bpp), p;
	u8 *lin = calloc(sz, 1), *out = calloc(sz, 1);

	for (size_t i = 0; i < (size_t)w * h; i++) {
		if (bpp == 3) {
			lin[i * 3] = src[i];
			lin[i * 3 + 1] = src[i] >> 8;
			lin[i * 3 + 2] = src[i] >> 16;
		} else {
			u16 v = pack565(src[i]);

			memcpy(lin + i * 2, &v, 2);
		}
	}
	for (p = 0; p < sz; p++)
		out[p ^ 4] = lin[p];
	free(lin);
	return out;
}

static int fail(const char *what, u32 w, u32 h, u32 bpp, size_t at)
{
	printf("FAIL %s: %ux%u bpp %u, first difference at byte %zu\n", what, w, h, bpp, at);
	return 1;
}

static int first_diff(const u8 *a, const u8 *b, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (a[i] != b[i])
			return (int)i;
	return -1;
}

static int test_case(u32 w, u32 h, u32 bpp)
{
	size_t sz = frame_size(w, h, bpp);
	u32 *src = malloc((size_t)w * h * 4), *src2 = malloc((size_t)w * h * 4);
	struct fl2000_stream st = { .width = w, .height = h, .bytes_pix = bpp, .buf_size = sz };
	u8 *want, *want2, *shadow = calloc(sz, 1), *buf;
	int d;

	for (size_t i = 0; i < (size_t)w * h; i++)
		src[i] = src2[i] = rnd();
	want = ref_convert(src, w, h, bpp);

	/* 1. converters, with the line cut in random spans */
	for (u32 y = 0; y < h; y++) {
		for (u32 x = 0; x < w;) {
			u32 n = 1 + rndn(w - x);

			if (bpp == 3)
				fl2000_xrgb8888_to_rgb888_line(shadow, (size_t)y * w * 3,
							       &src[(size_t)y * w + x], x, x + n);
			else
				fl2000_xrgb8888_to_rgb565_line((u16 *)shadow, (size_t)y * w,
							       &src[(size_t)y * w + x], x, x + n);
			x += n;
		}
	}
	d = first_diff(shadow, want, sz);
	if (d >= 0)
		return fail("converter", w, h, bpp, d);

	/* 2. rectangle copy: frame 2 differs from frame 1 only inside a rectangle */
	{
		struct drm_rect r = { (int)rndn(w + 8) - 4, (int)rndn(h + 8) - 4, 0, 0 };

		r.x2 = r.x1 + 1 + rndn(w);
		r.y2 = r.y1 + 1 + rndn(h);
		for (int y = max(r.y1, 0); y < min(r.y2, (int)h); y++)
			for (int x = max(r.x1, 0); x < min(r.x2, (int)w); x++)
				src2[(size_t)y * w + x] = rnd();
		want2 = ref_convert(src2, w, h, bpp);
		buf = malloc(sz);
		memcpy(buf, want, sz);
		st.shadow = want2;
		fl2000_copy_rect(&st, buf, &r);
		d = first_diff(buf, want2, sz);
		if (d >= 0)
			return fail("copy_rect", w, h, bpp, d);
		free(buf);
		free(want2);
	}

	free(src); free(src2); free(want); free(shadow);
	return 0;
}

/* The frame shown is a window of a larger framebuffer, and only part of it changed */
static int test_compress(u32 w, u32 h, u32 bpp, bool wc, u32 format)
{
	u32 sx = rndn(40), sy = rndn(9), fb_w = w + sx + rndn(40), fb_h = h + sy + rndn(9);
	u32 pitch = (fb_w + rndn(20)) * 4;
	size_t fb_size = (size_t)pitch * fb_h + 16, sz = frame_size(w, h, bpp);
	u32 *fb = malloc(fb_size), *frame = malloc((size_t)w * h * 4);
	struct fl2000_stream st = { .width = w, .height = h, .bytes_pix = bpp, .buf_size = sz };
	struct iosys_map map = { .vaddr = fb, .is_iomem = wc };
	struct drm_rect src = { sx, sy, sx + w, sy + h };
	struct drm_rect clip = { rndn(fb_w), rndn(fb_h), 0, 0 };
	u32 line_buf[4096];
	u8 *want;
	int d;

	clip.x2 = clip.x1 + 1 + rndn(fb_w - clip.x1);
	clip.y2 = clip.y1 + 1 + rndn(fb_h - clip.y1);

	for (size_t i = 0; i < fb_size / 4; i++)
		fb[i] = rnd();

	#define PIX(x, y) fb[(size_t)(y) * (pitch / 4) + (x)]
	#define FROM_FB(p) (format == DRM_FORMAT_XRGB8888 ? (p) : \
			    ((((p) >> 22) & 0xFF) << 16) | ((((p) >> 12) & 0xFF) << 8) | (((p) >> 2) & 0xFF))

	/* What the frame held before the update: outside the damage it must stay */
	for (u32 y = 0; y < h; y++)
		for (u32 x = 0; x < w; x++)
			frame[(size_t)y * w + x] = FROM_FB(PIX(sx + x, sy + y));
	st.shadow = ref_convert(frame, w, h, bpp);
	st.line_buf = line_buf;

	/* The update */
	for (int y = clip.y1; y < clip.y2; y++)
		for (int x = clip.x1; x < clip.x2; x++)
			PIX(x, y) = rnd();
	for (u32 y = 0; y < h; y++)
		for (u32 x = 0; x < w; x++)
			frame[(size_t)y * w + x] = FROM_FB(PIX(sx + x, sy + y));
	want = ref_convert(frame, w, h, bpp);

	fl2000_stream_compress(&st, &map, wc, format, pitch, &src, &clip);

	d = first_diff(st.shadow, want, sz);
	if (d >= 0)
		return fail("compress", w, h, bpp, d);

	free(st.shadow); free(want); free(frame); free(fb);
	return 0;
}

int main(void)
{
	static const u32 widths[] = { 1, 2, 3, 5, 6, 7, 8, 9, 15, 16, 17, 31, 63, 64, 65, 100, 854,
				      1024, 1360, 1366, 1368 };
	long cases = 0;

	for (int i = 0; i < 4000; i++) {
		u32 w = i % 3 ? widths[rndn(sizeof(widths) / sizeof(*widths))] : 1 + rndn(130);
		u32 h = 1 + rndn(10), bpp = 2 + (rnd() & 1);

		if (test_case(w, h, bpp))
			return 1;
		if (test_compress(w, h, bpp, rnd() & 1,
				  rnd() & 1 ? DRM_FORMAT_XRGB8888 : DRM_FORMAT_XRGB2101010))
			return 1;
		cases++;
	}
	/* The native mode of the VGA test monitor and the common HDMI one */
	for (int b = 2; b <= 3; b++) {
		if (test_case(1366, 768, b) || test_case(1920, 1080, b))
			return 1;
		cases += 2;
	}
	printf("OK: %ld cases (converters, rectangle copy, damage conversion)\n", cases);
	return 0;
}
'''


def extract(src, name):
    m = re.search(r'^(?:static |void )[^\n;]*\b%s\(' % re.escape(name), src, re.M)
    if not m:
        sys.exit('function not found in %s: %s' % (SRC, name))
    end = src.index('\n}\n', m.start()) + 3
    return src[m.start():end]


def main():
    mutate = '--mutate' in sys.argv
    with open(SRC) as f:
        src = f.read()
    body = '\n'.join(extract(src, n) for n in FUNCS)
    if mutate:
        if 'round_down((size_t)y * line_len + off, 8)' not in body:
            sys.exit('mutation anchor not found')
        body = body.replace('round_down((size_t)y * line_len + off, 8)',
                            '((size_t)y * line_len + off)')
        body = body.replace('round_up((size_t)y * line_len + off + span, 8)',
                            '((size_t)y * line_len + off + span)')
    with tempfile.TemporaryDirectory() as tmp:
        c = os.path.join(tmp, 'layout.c')
        exe = os.path.join(tmp, 'layout')
        with open(c, 'w') as f:
            f.write(PRELUDE + body + MAIN)
        subprocess.run(['cc', '-O1', '-g', '-Wall', '-Werror', '-Wno-pointer-arith',
                        '-Wno-unused-function', '-fsanitize=address,undefined', '-o', exe, c],
                       check=True)
        res = subprocess.run([exe])
    if mutate:
        print('mutation %s' % ('NOTICED, as it should' if res.returncode else 'NOT noticed!'))
        sys.exit(0 if res.returncode else 1)
    sys.exit(res.returncode)


if __name__ == '__main__':
    main()
