// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace side of the VM tests. Exit status 0 is the expected result.
 *
 *   fl2000-t hugemode <card>            a huge mode with an off-screen primary plane: must be rejected
 *   fl2000-t show <card> <w> <h> <fbw> <fbh> <sx> <sy> <secs> <fmt>   atomic commit with a pattern
 *   fl2000-t failnth <card> <max>       mode sets with allocation failures injected
 *   fl2000-t reset <busnum> <devnum>    USBDEVFS_RESET
 *   fl2000-t i2c0 <dev>                 zero-length and write-write I2C_RDWR
 *   fl2000-t hold <card> <secs>         hold the card open
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/usbdevice_fs.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static int fd;
static uint32_t conn_id, crtc_id, plane_id;

static uint32_t prop(uint32_t obj, uint32_t type, const char *name)
{
	drmModeObjectProperties *p = drmModeObjectGetProperties(fd, obj, type);
	uint32_t id = 0;

	for (uint32_t i = 0; p && i < p->count_props && !id; i++) {
		drmModePropertyRes *r = drmModeGetProperty(fd, p->props[i]);

		if (r && !strcmp(r->name, name))
			id = r->prop_id;
		drmModeFreeProperty(r);
	}
	drmModeFreeObjectProperties(p);
	if (!id)
		fprintf(stderr, "no property %s\n", name);
	return id;
}

static int open_card(const char *path)
{
	drmModeRes *res;
	drmModePlaneRes *pl;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(path);
		exit(2);
	}
	drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
		perror("atomic cap");
		exit(2);
	}
	res = drmModeGetResources(fd);
	if (!res) {
		perror("resources");
		exit(2);
	}
	crtc_id = res->crtcs[0];
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);

		if (c && c->connection == DRM_MODE_CONNECTED && !conn_id)
			conn_id = c->connector_id;
		drmModeFreeConnector(c);
	}
	pl = drmModeGetPlaneResources(fd);
	for (uint32_t i = 0; pl && i < pl->count_planes; i++) {
		drmModeObjectProperties *p = drmModeObjectGetProperties(fd, pl->planes[i], DRM_MODE_OBJECT_PLANE);

		for (uint32_t j = 0; p && j < p->count_props; j++) {
			drmModePropertyRes *r = drmModeGetProperty(fd, p->props[j]);

			if (r && !strcmp(r->name, "type")) {
				if (p->prop_values[j] == DRM_PLANE_TYPE_PRIMARY)
					plane_id = pl->planes[i];
			}
			drmModeFreeProperty(r);
		}
		drmModeFreeObjectProperties(p);
	}
	if (!conn_id || !plane_id) {
		fprintf(stderr, "no connected connector or no plane (%u %u)\n", conn_id, plane_id);
		exit(2);
	}
	return 0;
}

struct fbuf {
	uint32_t id, pitch, handle;
	uint32_t *map;
	size_t size;
};

static int make_fb(struct fbuf *f, uint32_t w, uint32_t h, uint32_t fourcc)
{
	struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = 32 };
	struct drm_mode_map_dumb m = { 0 };
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };

	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c))
		return -errno;
	m.handle = c.handle;
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m))
		return -errno;
	f->map = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
	if (f->map == MAP_FAILED)
		return -errno;
	f->size = c.size;
	f->pitch = c.pitch;
	f->handle = c.handle;
	handles[0] = c.handle;
	pitches[0] = c.pitch;
	if (drmModeAddFB2(fd, w, h, fourcc, handles, pitches, offsets, &f->id, 0))
		return -errno;
	return 0;
}

static void make_mode(drmModeModeInfo *m, int w, int h, int clock)
{
	memset(m, 0, sizeof(*m));
	m->hdisplay = w;
	m->hsync_start = w + 8;
	m->hsync_end = w + 16;
	m->htotal = w + 32;
	m->vdisplay = h;
	m->vsync_start = h + 1;
	m->vsync_end = h + 2;
	m->vtotal = h + 8;
	m->clock = clock;
	m->vrefresh = 60;
	m->type = DRM_MODE_TYPE_USERDEF;
	snprintf(m->name, sizeof(m->name), "%dx%d", w, h);
}

static int commit(drmModeModeInfo *mode, struct fbuf *f, int sx, int sy, int w, int h,
		  int crtc_x, uint32_t flags)
{
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	uint32_t blob = 0;
	int ret;

	if (mode) {
		if (drmModeCreatePropertyBlob(fd, mode, sizeof(*mode), &blob))
			return -errno;
		drmModeAtomicAddProperty(req, crtc_id, prop(crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID"), blob);
		drmModeAtomicAddProperty(req, crtc_id, prop(crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE"), 1);
		drmModeAtomicAddProperty(req, conn_id, prop(conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"), crtc_id);
	}
	drmModeAtomicAddProperty(req, plane_id, prop(plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID"), f->id);
	drmModeAtomicAddProperty(req, plane_id, prop(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID"), crtc_id);
	drmModeAtomicAddProperty(req, plane_id, prop(plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X"), (uint64_t)sx << 16);
	drmModeAtomicAddProperty(req, plane_id, prop(plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y"), (uint64_t)sy << 16);
	drmModeAtomicAddProperty(req, plane_id, prop(plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W"), (uint64_t)w << 16);
	drmModeAtomicAddProperty(req, plane_id, prop(plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H"), (uint64_t)h << 16);
	drmModeAtomicAddProperty(req, plane_id, prop(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X"), crtc_x);
	drmModeAtomicAddProperty(req, plane_id, prop(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y"), 0);
	drmModeAtomicAddProperty(req, plane_id, prop(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W"), w);
	drmModeAtomicAddProperty(req, plane_id, prop(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H"), h);
	ret = drmModeAtomicCommit(fd, req, flags, NULL);
	if (ret)
		ret = -errno;
	drmModeAtomicFree(req);
	return ret;
}

static int disable_all(void)
{
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	int ret;

	drmModeAtomicAddProperty(req, crtc_id, prop(crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID"), 0);
	drmModeAtomicAddProperty(req, crtc_id, prop(crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE"), 0);
	drmModeAtomicAddProperty(req, conn_id, prop(conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"), 0);
	drmModeAtomicAddProperty(req, plane_id, prop(plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID"), 0);
	drmModeAtomicAddProperty(req, plane_id, prop(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID"), 0);
	ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	if (ret)
		ret = -errno;
	drmModeAtomicFree(req);
	return ret;
}

static int cmd_hugemode(void)
{
	struct fbuf f;
	drmModeModeInfo m;
	int ret;

	ret = make_fb(&f, 64, 64, DRM_FORMAT_XRGB8888);
	if (ret)
		return fprintf(stderr, "fb: %s\n", strerror(-ret)), 2;
	/* Wraps the 32-bit buffer size to 32 kB */
	make_mode(&m, 43691, 32768, 25000);
	ret = commit(&m, &f, 0, 0, 64, 64, 60000, DRM_MODE_ATOMIC_ALLOW_MODESET);
	printf("huge mode with off-screen primary: %s\n", ret ? strerror(-ret) : "ACCEPTED");
	if (!ret) {
		fflush(stdout);
		sleep(3);
		disable_all();
		return 1;
	}
	/* Sanity: the same commit path takes a mode that is fine */
	make_mode(&m, 1024, 768, 65000);
	ret = make_fb(&f, 1024, 768, DRM_FORMAT_XRGB8888);
	if (!ret)
		ret = commit(&m, &f, 0, 0, 1024, 768, 0, DRM_MODE_ATOMIC_ALLOW_MODESET);
	printf("normal mode through the same path: %s\n", ret ? strerror(-ret) : "accepted");
	disable_all();
	return ret ? 3 : 0;
}

/* Every pixel tells where it is in the framebuffer */
static uint32_t pattern(uint32_t x, uint32_t y)
{
	return ((x * 7 + y * 13) & 0xff) | (((x >> 3) & 0xff) << 8) | ((y & 0xff) << 16);
}

static int cmd_show(char **a)
{
	int w = atoi(a[0]), h = atoi(a[1]), fw = atoi(a[2]), fh = atoi(a[3]);
	int sx = atoi(a[4]), sy = atoi(a[5]), secs = atoi(a[6]);
	uint32_t fourcc = !strcmp(a[7], "ar30") ? DRM_FORMAT_ARGB2101010 : DRM_FORMAT_XRGB8888;
	struct fbuf f;
	drmModeModeInfo m;
	int ret;

	ret = make_fb(&f, fw, fh, fourcc);
	if (ret)
		return fprintf(stderr, "fb: %s\n", strerror(-ret)), 2;
	for (int y = 0; y < fh; y++)
		for (int x = 0; x < fw; x++) {
			uint32_t p = pattern(x, y);

			if (fourcc == DRM_FORMAT_ARGB2101010)
				p = 0xc0000000 | ((p >> 16 & 0xff) << 22) | ((p >> 8 & 0xff) << 12) | ((p & 0xff) << 2);
			f.map[(size_t)y * (f.pitch / 4) + x] = p;
		}
	/* A clock the PLL makes exactly (10 MHz * 5 / 2 or * 40 / 4), for the 500 ppm of HDMI */
	make_mode(&m, w, h, w * h <= 640 * 480 ? 25000 : 100000);
	ret = commit(&m, &f, sx, sy, w, h, 0, DRM_MODE_ATOMIC_ALLOW_MODESET);
	if (ret)
		return fprintf(stderr, "commit %dx%d: %s\n", w, h, strerror(-ret)), 1;
	printf("showing %dx%d from a %dx%d framebuffer at %d,%d\n", w, h, fw, fh, sx, sy);
	fflush(stdout);
	sleep(secs);
	return 0;
}

static int cmd_failnth(int start, int max)
{
	struct fbuf small, big;
	drmModeModeInfo ms, mb;
	char path[64];
	int ctl, injected = 0, ok = 0, failed = 0, first_clean = 0;

	if (make_fb(&small, 640, 480, DRM_FORMAT_XRGB8888) || make_fb(&big, 1920, 1080, DRM_FORMAT_XRGB8888))
		return 2;
	make_mode(&ms, 640, 480, 25000);
	make_mode(&mb, 1920, 1080, 100000);
	snprintf(path, sizeof(path), "/proc/thread-self/fail-nth");

	for (int n = start; n <= max; n++) {
		char buf[16];
		int ret;

		/* Small mode, without faults: the old buffers the failure must not reuse */
		if (commit(&ms, &small, 0, 0, 640, 480, 0, DRM_MODE_ATOMIC_ALLOW_MODESET)) {
			fprintf(stderr, "small mode failed: %s\n", strerror(errno));
			return 1;
		}

		ctl = open(path, O_RDWR);
		if (ctl < 0)
			return perror(path), 2;
		snprintf(buf, sizeof(buf), "%d", n);
		if (write(ctl, buf, strlen(buf)) < 0)
			return perror("fail-nth"), 2;
		ret = commit(&mb, &big, 0, 0, 1920, 1080, 0, DRM_MODE_ATOMIC_ALLOW_MODESET);
		memset(buf, 0, sizeof(buf));
		lseek(ctl, 0, SEEK_SET);
		if (read(ctl, buf, sizeof(buf)) > 0 && atoi(buf) == 0)
			injected++;
		close(ctl);
		if (ret)
			failed++;
		else
			ok++;

		/* The next update and frame reach whatever state the failure left */
		memset(big.map, n, big.size);
		drmModeDirtyFB(fd, big.id, NULL, 0);
		usleep(2000);
		if (atoi(buf) != 0 && ++first_clean > 3) {
			printf("nothing left to inject at n=%d\n", n);
			break;
		}
	}
	printf("fail-nth: %d injected, commits failed %d, passed %d\n", injected, failed, ok);
	disable_all();
	return 0;
}

/* Two framebuffers, flipped the way a compositor does: nonblocking, with events */
static int flips_done;

static void on_flip(int fd_, unsigned int seq, unsigned int sec, unsigned int usec, void *data)
{
	flips_done++;
}

static int cmd_flips(int secs)
{
	struct fbuf a, b;
	drmModeModeInfo m;
	drmEventContext ev = { .version = 2, .page_flip_handler = on_flip };
	struct timespec t0, t1;
	int sent = 0, ret, n = 0;
	double el;

	if (make_fb(&a, 1024, 768, DRM_FORMAT_XRGB8888) || make_fb(&b, 1024, 768, DRM_FORMAT_XRGB8888))
		return 2;
	memset(a.map, 0x20, a.size);
	memset(b.map, 0xe0, b.size);
	make_mode(&m, 1024, 768, 65000);
	ret = commit(&m, &a, 0, 0, 1024, 768, 0, DRM_MODE_ATOMIC_ALLOW_MODESET);
	if (ret)
		return fprintf(stderr, "commit: %s\n", strerror(-ret)), 1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	do {
		ret = commit(NULL, n & 1 ? &a : &b, 0, 0, 1024, 768, 0, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT);
		/* The event carries user data 0: one flip at a time */
		if (ret) {
			fprintf(stderr, "flip %d: %s\n", n, strerror(-ret));
			return 1;
		}
		sent++;
		n++;
		while (flips_done < sent) {
			fd_set fds;
			struct timeval tv = { .tv_sec = 2 };

			FD_ZERO(&fds);
			FD_SET(fd, &fds);
			if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) {
				fprintf(stderr, "no flip event in 2 s after %d flips\n", flips_done);
				return 1;
			}
			drmHandleEvent(fd, &ev);
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	} while (el < secs);
	printf("atomic flips: %d in %.1f s, %.1f Hz (the mode is %.1f Hz)\n", flips_done, el, flips_done / el,
	       65000000.0 / (m.htotal * m.vtotal));
	return flips_done / el > 20 ? 0 : 1;
}

/* A damage clip on a framebuffer larger than the mode, partly outside of what it shows */
static int cmd_damage(char **a)
{
	int w = atoi(a[0]), h = atoi(a[1]), fw = atoi(a[2]), fh = atoi(a[3]);
	int sx = atoi(a[4]), sy = atoi(a[5]);
	struct drm_clip_rect clip = { atoi(a[6]), atoi(a[7]), atoi(a[8]), atoi(a[9]) };
	struct fbuf f;
	drmModeModeInfo m;
	int ret;

	ret = make_fb(&f, fw, fh, DRM_FORMAT_XRGB8888);
	if (ret)
		return fprintf(stderr, "fb: %s\n", strerror(-ret)), 2;
	for (int y = 0; y < fh; y++)
		for (int x = 0; x < fw; x++)
			f.map[(size_t)y * (f.pitch / 4) + x] = pattern(x, y);
	make_mode(&m, w, h, 100000);
	ret = commit(&m, &f, sx, sy, w, h, 0, DRM_MODE_ATOMIC_ALLOW_MODESET);
	if (ret)
		return fprintf(stderr, "commit: %s\n", strerror(-ret)), 1;
	sleep(1);
	for (int y = clip.y1; y < clip.y2; y++)
		for (int x = clip.x1; x < clip.x2; x++)
			f.map[(size_t)y * (f.pitch / 4) + x] = pattern(x + 1000, y + 1000);
	ret = drmModeDirtyFB(fd, f.id, &clip, 1);
	if (ret)
		return fprintf(stderr, "dirty: %s\n", strerror(-ret)), 1;
	printf("damaged %d,%d-%d,%d of the framebuffer\n", clip.x1, clip.y1, clip.x2, clip.y2);
	fflush(stdout);
	sleep(atoi(a[10]));
	return 0;
}

/* Commits as fast as they go, until the device goes away */
static int cmd_storm(int secs)
{
	struct fbuf f;
	drmModeModeInfo m;
	int errs = 0, n = 0;

	if (make_fb(&f, 1024, 768, DRM_FORMAT_XRGB8888))
		return 2;
	make_mode(&m, 1024, 768, 65000);
	for (time_t end = time(NULL) + secs; time(NULL) < end; n++) {
		int ret = commit(&m, &f, 0, 0, 1024, 768, 0, DRM_MODE_ATOMIC_ALLOW_MODESET);

		if (ret)
			errs++;
		drmModeDirtyFB(fd, f.id, NULL, 0);
		if (!(n % 3)) {
			disable_all();
		}
	}
	printf("storm: %d commits, %d errors\n", n, errs);
	return 0;
}

static int cmd_i2c0(const char *dev)
{
	int f = open(dev, O_RDWR);
	uint8_t a[2] = { 0, 0 }, b[2] = { 0, 0 };
	struct i2c_msg zero = { .addr = 0x4c, .flags = 0, .len = 0, .buf = a };
	struct i2c_msg ww[2] = {
		{ .addr = 0x4c, .flags = 0, .len = 1, .buf = a },
		{ .addr = 0x4c, .flags = 0, .len = 1, .buf = b },
	};
	struct i2c_msg rd[2] = {
		{ .addr = 0x4c, .flags = 0, .len = 1, .buf = a },
		{ .addr = 0x4c, .flags = I2C_M_RD, .len = 1, .buf = b },
	};
	struct i2c_rdwr_ioctl_data d;
	int r1, r2, r3;

	if (f < 0)
		return perror(dev), 2;
	d.msgs = &zero;
	d.nmsgs = 1;
	r1 = ioctl(f, I2C_RDWR, &d);
	printf("zero-length message: %s\n", r1 < 0 ? strerror(errno) : "accepted");
	d.msgs = ww;
	d.nmsgs = 2;
	r2 = ioctl(f, I2C_RDWR, &d);
	printf("two writes: %s\n", r2 < 0 ? strerror(errno) : "accepted");
	d.msgs = rd;
	d.nmsgs = 2;
	a[0] = 0;
	b[0] = 0;
	r3 = ioctl(f, I2C_RDWR, &d);
	printf("write then read (register 0): %s, value 0x%02x\n", r3 < 0 ? strerror(errno) : "accepted", b[0]);
	close(f);
	return (r1 < 0 && r2 < 0 && r3 >= 0 && b[0] == 0x54) ? 0 : 1;
}

int main(int argc, char **argv)
{
	if (argc < 3)
		return 2;
	if (!strcmp(argv[1], "reset")) {
		char p[64];
		int f;

		snprintf(p, sizeof(p), "/dev/bus/usb/%03d/%03d", atoi(argv[2]), atoi(argv[3]));
		f = open(p, O_WRONLY);
		if (f < 0)
			return perror(p), 2;
		if (ioctl(f, USBDEVFS_RESET, 0)) {
			perror("USBDEVFS_RESET");
			return 1;
		}
		return 0;
	}
	if (!strcmp(argv[1], "resetfail")) {
		char p[64], buf[16];
		int f, ctl, fails = 0, inj = 0;

		snprintf(p, sizeof(p), "/dev/bus/usb/%03d/%03d", atoi(argv[2]), atoi(argv[3]));
		f = open(p, O_WRONLY);
		if (f < 0)
			return perror(p), 2;
		for (int n = 1; n <= atoi(argv[4]); n++) {
			ctl = open("/proc/thread-self/fail-nth", O_RDWR);
			snprintf(buf, sizeof(buf), "%d", n);
			if (ctl < 0 || write(ctl, buf, strlen(buf)) < 0)
				return perror("fail-nth"), 2;
			if (ioctl(f, USBDEVFS_RESET, 0))
				fails++;
			memset(buf, 0, sizeof(buf));
			lseek(ctl, 0, SEEK_SET);
			if (read(ctl, buf, sizeof(buf)) > 0 && atoi(buf) == 0)
				inj++;
			close(ctl);
		}
		printf("reset with faults: %d injected, %d resets failed\n", inj, fails);
		return 0;
	}
	if (!strcmp(argv[1], "i2c0"))
		return cmd_i2c0(argv[2]);
	if (!strcmp(argv[1], "hold")) {
		int f = open(argv[2], O_RDWR);

		if (f < 0)
			return perror(argv[2]), 2;
		sleep(atoi(argv[3]));
		close(f);
		return 0;
	}
	open_card(argv[2]);
	if (!strcmp(argv[1], "hugemode"))
		return cmd_hugemode();
	if (!strcmp(argv[1], "show"))
		return cmd_show(argv + 3);
	if (!strcmp(argv[1], "failnth"))
		return cmd_failnth(atoi(argv[3]), atoi(argv[4]));
	if (!strcmp(argv[1], "flips"))
		return cmd_flips(atoi(argv[3]));
	if (!strcmp(argv[1], "damage"))
		return cmd_damage(argv + 3);
	if (!strcmp(argv[1], "storm"))
		return cmd_storm(atoi(argv[3]));
	return 2;
}
