/* SPDX-License-Identifier: GPL-2.0 */
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#ifndef FL2000_H
#define FL2000_H

#include <linux/component.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/usb.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_crtc.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_plane.h>
#include <drm/drm_encoder.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_vblank.h>

#include "fl2000_registers.h"

/* Known USB interfaces of FL2000 */
enum fl2000_interface {
	FL2000_USBIF_AVCONTROL = 0,
	FL2000_USBIF_STREAMING = 1,
	FL2000_USBIF_INTERRUPT = 2,
};

/* Process context only: may sleep between retries */
static inline int fl2000_submit_urb(struct urb *urb)
{
	int ret;
	int attempts = 10;

	do {
		ret = usb_submit_urb(urb, GFP_KERNEL);
		switch (ret) {
		case -ENXIO:
		case -ENOMEM:
			if (attempts--) {
				cond_resched();
				ret = -EAGAIN;
			}
			break;
		default:
			break;
		}
	} while (ret == -EAGAIN);

	return ret;
}

/* Called from URB completion handlers, i.e. in atomic context */
static inline int fl2000_urb_status(struct usb_device *usb_dev, int status)
{
	switch (status) {
	case 0:
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* Success, or the URB was unlinked / the device is going away */
		break;
	default:
		dev_err_ratelimited(&usb_dev->dev, "Nonzero urb status, %d\n", status);
		break;
	}

	return status;
}

struct fl2000_timings {
	u32 hactive;
	u32 htotal;
	u32 hsync_width;
	u32 hstart;
	u32 vactive;
	u32 vtotal;
	u32 vsync_width;
	u32 vstart;
};

struct fl2000_pll {
	u32 prescaler;
	u32 multiplier;
	u32 divisor;
	u32 function;
};

/* Polling interval and timeout in us for I2C read/write operations */
#define I2C_RDWR_INTERVAL 200
#define I2C_RDWR_TIMEOUT  (256 * 1000)

/* Streaming transfer task */
struct fl2000_stream;
struct fl2000_stream *fl2000_stream_create(struct usb_device *usb_dev, struct drm_crtc *crtc);
void fl2000_stream_destroy(struct usb_device *usb_dev);

/* Streaming interface */
int fl2000_stream_mode_set(struct fl2000_stream *stream, int pixels, u32 bytes_pix);
void fl2000_stream_compress(struct fl2000_stream *stream, void *src, unsigned int height,
			    unsigned int width, unsigned int pitch);
int fl2000_stream_enable(struct fl2000_stream *stream);
void fl2000_stream_disable(struct fl2000_stream *stream);

/* Interrupt polling task */
struct fl2000_intr;
struct fl2000_intr *fl2000_intr_create(struct usb_device *usb_dev, struct drm_device *drm);
void fl2000_intr_destroy(struct usb_device *usb_dev);

/* I2C adapter interface creation */
struct i2c_adapter *fl2000_i2c_init(struct usb_device *usb_dev);

/* Register map creation */
struct regmap *fl2000_regmap_init(struct usb_device *usb_dev);

/* Registers interface */
int fl2000_reset(struct usb_device *usb_dev);
int fl2000_usb_magic(struct usb_device *usb_dev);
int fl2000_afe_magic(struct usb_device *usb_dev);
int fl2000_set_transfers(struct usb_device *usb_dev);
int fl2000_set_pixfmt(struct usb_device *usb_dev, u32 bytes_pix);
int fl2000_set_timings(struct usb_device *usb_dev, struct fl2000_timings *timings);
int fl2000_set_pll(struct usb_device *usb_dev, struct fl2000_pll *pll);
int fl2000_enable_interrupts(struct usb_device *usb_dev);
int fl2000_check_interrupt(struct usb_device *usb_dev);
int fl2000_i2c_dword(struct usb_device *usb_dev, bool read, u16 addr, u8 offset, u32 *data);

/* DRM device creation */
int fl2000_drm_bind(struct device *master);
void fl2000_drm_unbind(struct device *master);

#endif /* FL2000_H */
