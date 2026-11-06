// SPDX-License-Identifier: GPL-2.0
/*
 * Save the framebuffer the compositor is scanning out on the fl2000 primary
 * plane as a PPM image: what the driver is given to show, before any of its
 * own conversion. Tells a broken picture made by the compositor from one made
 * by the driver.
 *
 *   gcc -O2 -o /tmp/fl2000-dump-fb scripts/fl2000-dump-fb.c $(pkg-config --cflags --libs libdrm)
 *   sudo /tmp/fl2000-dump-fb out.ppm
 *
 * Needs root: DRM_IOCTL_MODE_GETFB2 only returns buffer handles to
 * CAP_SYS_ADMIN.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static int open_fl2000(void)
{
	for (int i = 0; i < 16; i++) {
		char path[32];
		drmVersionPtr ver;
		int fd;

		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		ver = drmGetVersion(fd);
		if (ver && !strcmp(ver->name, "fl2000")) {
			drmFreeVersion(ver);
			fprintf(stderr, "device %s\n", path);
			return fd;
		}
		drmFreeVersion(ver);
		close(fd);
	}
	return -1;
}

int main(int argc, char **argv)
{
	drmModePlaneResPtr res;
	drmModeFB2Ptr fb = NULL;
	uint32_t fb_id = 0;
	int fd, dmabuf;
	size_t size;
	uint8_t *map;
	FILE *out;

	if (argc != 2) {
		fprintf(stderr, "usage: %s out.ppm\n", argv[0]);
		return 2;
	}

	fd = open_fl2000();
	if (fd < 0) {
		fprintf(stderr, "no fl2000 DRM device\n");
		return 1;
	}
	drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

	res = drmModeGetPlaneResources(fd);
	for (uint32_t i = 0; res && i < res->count_planes && !fb_id; i++) {
		drmModePlanePtr plane = drmModeGetPlane(fd, res->planes[i]);
		drmModeObjectPropertiesPtr props;

		if (!plane)
			continue;
		props = drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
		for (uint32_t j = 0; props && j < props->count_props; j++) {
			drmModePropertyPtr p = drmModeGetProperty(fd, props->props[j]);

			if (p && !strcmp(p->name, "type") &&
			    props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY)
				fb_id = plane->fb_id;
			drmModeFreeProperty(p);
		}
		drmModeFreeObjectProperties(props);
		drmModeFreePlane(plane);
	}
	if (!fb_id) {
		fprintf(stderr, "primary plane has no framebuffer\n");
		return 1;
	}

	fb = drmModeGetFB2(fd, fb_id);
	if (!fb || !fb->handles[0]) {
		fprintf(stderr, "GETFB2 %u failed (root needed): %s\n", fb_id, strerror(errno));
		return 1;
	}
	fprintf(stderr, "fb %u: %ux%u format %.4s pitch %u offset %u modifier 0x%llx\n", fb_id,
		fb->width, fb->height, (char *)&fb->pixel_format, fb->pitches[0], fb->offsets[0],
		(unsigned long long)fb->modifier);

	if (drmPrimeHandleToFD(fd, fb->handles[0], DRM_CLOEXEC | DRM_RDWR, &dmabuf) &&
	    drmPrimeHandleToFD(fd, fb->handles[0], DRM_CLOEXEC, &dmabuf)) {
		fprintf(stderr, "PRIME export failed: %s\n", strerror(errno));
		return 1;
	}
	size = (size_t)fb->offsets[0] + (size_t)fb->pitches[0] * fb->height;
	map = mmap(NULL, size, PROT_READ, MAP_SHARED, dmabuf, 0);
	if (map == MAP_FAILED) {
		fprintf(stderr, "mmap of the dma-buf failed: %s\n", strerror(errno));
		return 1;
	}

	out = fopen(argv[1], "wb");
	if (!out)
		return 1;
	fprintf(out, "P6\n%u %u\n255\n", fb->width, fb->height);
	for (uint32_t y = 0; y < fb->height; y++) {
		const uint32_t *line = (const uint32_t *)(map + fb->offsets[0] +
							  (size_t)y * fb->pitches[0]);

		for (uint32_t x = 0; x < fb->width; x++) {
			uint32_t p = line[x];
			uint8_t rgb[3];

			if (fb->pixel_format == DRM_FORMAT_XRGB2101010 ||
			    fb->pixel_format == DRM_FORMAT_ARGB2101010) {
				rgb[0] = p >> 22;
				rgb[1] = p >> 12;
				rgb[2] = p >> 2;
			} else {
				rgb[0] = p >> 16;
				rgb[1] = p >> 8;
				rgb[2] = p;
			}
			fwrite(rgb, 1, 3, out);
		}
	}
	fclose(out);
	fprintf(stderr, "saved %s\n", argv[1]);
	return 0;
}
