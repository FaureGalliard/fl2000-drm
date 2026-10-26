// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2012, Red Hat
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include <linux/math64.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_vblank_helper.h>

#include "fl2000.h"

#define DRM_DRIVER_NAME "fl2000"
#define DRM_DRIVER_DESC "Fresco Logic FL2000 USB display"

#define DRM_DRIVER_MAJOR      0
#define DRM_DRIVER_MINOR      0
#define DRM_DRIVER_PATCHLEVEL 1

/* Maximum supported resolution, no documented hardware limit */
#define FL2000_MAX_WIDTH  4000
#define FL2000_MAX_HEIGHT 4000

/* Force using 32-bit XRGB8888 on input for simplicity */
#define FL2000_FB_BPP 32
static const u32 fl2000_pixel_formats[] = {
	DRM_FORMAT_XRGB8888,
};

/*
 * Maximum pixel clock set to 500MHz. It is hard to get more or less precise PLL configuration for
 * higher clock.
 */
#define FL2000_MAX_PIXCLOCK 500000000

/* PLL computing precision is 6 digits after the decimal point */
#define FL2000_PLL_PRECISION 1000000

/* Input xtal clock, Hz */
#define FL2000_XTAL 10000000 /* 10 MHz */

/* Internal vco clock min/max, Hz */
#define FL2000_VCOCLOCK_MIN 62500000 /* 62.5 MHz */
#define FL2000_VCOCLOCK_MAX 1000000000 /* 1 GHz */

/* Maximum acceptable ppm error */
#define FL2000_PPM_ERR_MAX 500

/* Assume bulk transfers can use only 80% of USB bandwidth */
#define FL2000_BULK_BW_PERCENT 80

#define FL2000_BULK_BW_HIGH_SPEED	(480000000ull * 100 / FL2000_BULK_BW_PERCENT / 8)
#define FL2000_BULK_BW_SUPER_SPEED	(5000000000ull * 100 / FL2000_BULK_BW_PERCENT / 8)
#define FL2000_BULK_BW_SUPER_SPEED_PLUS (10000000000ull * 100 / FL2000_BULK_BW_PERCENT / 8)

static unsigned int fl2000_get_bytes_pix(enum usb_device_speed speed, unsigned int pixclock)
{
	unsigned int bytes_pix;
	unsigned int max_bw;

	/* Calculate maximum bandwidth, bytes per second */
	switch (speed) {
	case USB_SPEED_HIGH:
		max_bw = FL2000_BULK_BW_HIGH_SPEED;
		break;
	case USB_SPEED_SUPER:
		max_bw = FL2000_BULK_BW_SUPER_SPEED;
		break;
	case USB_SPEED_SUPER_PLUS:
		max_bw = FL2000_BULK_BW_SUPER_SPEED_PLUS;
		break;
	default:
		return 0;
	}

	/* Maximum bytes per pixel with maximum bandwidth */
	bytes_pix = max_bw / pixclock;
	switch (bytes_pix) {
	case 0: /* Not enough */
	case 1: /* RGB 332 not supported */
		return 0;
	case 2: /* RGB 565 */
	case 3: /* RGB 888 */
		break;
	default:
		bytes_pix = 3;
		break;
	}

	return bytes_pix;
}

struct fl2000_drm_if {
	struct usb_device *usb_dev;
	struct drm_device drm;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct fl2000_stream *stream;
	struct fl2000_intr *intr;
};

static inline struct fl2000_drm_if *to_fl2000_drm_if(struct drm_device *drm)
{
	return container_of(drm, struct fl2000_drm_if, drm);
}

DEFINE_DRM_GEM_FOPS(fl2000_drm_driver_fops);

/*
 * Mode config cleanup is managed by drmm_mode_config_init(), and the display
 * pipeline is shut down on unbind, so no .release callback is needed.
 */
static struct drm_driver fl2000_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.fops = &fl2000_drm_driver_fops,

	/*
	 * Paged shmem buffers: the framebuffer is only ever read by the CPU
	 * (fl2000_stream_compress), never DMA'd to the device, so it does not
	 * need to be physically contiguous.
	 */
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,

	.name = DRM_DRIVER_NAME,
	.desc = DRM_DRIVER_DESC,
	.major = DRM_DRIVER_MAJOR,
	.minor = DRM_DRIVER_MINOR,
	.patchlevel = DRM_DRIVER_PATCHLEVEL,
};

static const struct drm_mode_config_funcs fl2000_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* Integer division compute of ppm error */
static u64 fl2000_pll_ppm_err(u64 clock_mil, u32 vco_clk, u32 divisor)
{
	u64 pll_clk_mil = div_u64((u64)vco_clk * FL2000_PLL_PRECISION, divisor);
	u64 pll_clk_err;

	/* Not using abs() here to avoid a possible overflow */
	if (pll_clk_mil > clock_mil)
		pll_clk_err = pll_clk_mil - clock_mil;
	else
		pll_clk_err = clock_mil - pll_clk_mil;

	return div64_u64(pll_clk_err, div_u64(clock_mil, FL2000_PLL_PRECISION));
}

static inline u32 fl2000_pll_get_divisor(u64 clock_mil, u32 vco_clk, u64 *min_ppm_err)
{
	static const u32 divisor_arr[] = {
		2,   4,	  6,   7,   8,	 9,   10,  11,	12,  13,  14,  15,  16,	 17,  18,  19,
		20,  21,  22,  23,  24,	 25,  26,  27,	28,  29,  30,  31,  32,	 33,  34,  35,
		36,  37,  38,  39,  40,	 41,  42,  43,	44,  45,  46,  47,  48,	 49,  50,  51,
		52,  53,  54,  55,  56,	 57,  58,  59,	60,  61,  62,  63,  64,	 65,  66,  67,
		68,  69,  70,  71,  72,	 73,  74,  75,	76,  77,  78,  79,  80,	 81,  82,  83,
		84,  85,  86,  87,  88,	 89,  90,  91,	92,  93,  94,  95,  96,	 97,  98,  99,
		100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115,
		116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128
	};
	u32 best_divisor = 0;

	for (int i = 0; i < ARRAY_SIZE(divisor_arr); i++) {
		u32 divisor = divisor_arr[i];
		u64 ppm_err = fl2000_pll_ppm_err(clock_mil, vco_clk, divisor);

		if (ppm_err < *min_ppm_err) {
			*min_ppm_err = ppm_err;
			best_divisor = divisor;
		}
	}

	return best_divisor;
}

/* Try to match pixel clock - find parameters with minimal PLL error */
static u64 fl2000_pll_calc(u64 clock_mil, struct fl2000_pll *pll, u32 *clock_calculated)
{
	static const u32 prescaler_max = 2;
	static const u32 multiplier_max = 128;
	u64 min_ppm_err = (u64)(-1);

	for (u32 prescaler = 1; prescaler <= prescaler_max; prescaler++)
		for (u32 multiplier = 1; multiplier <= multiplier_max; multiplier++) {
			/* Do not need precision here yet, no 10^6 multiply */
			u32 vco_clk = FL2000_XTAL / prescaler * multiplier;
			u32 divisor;

			if (vco_clk < FL2000_VCOCLOCK_MIN || vco_clk > FL2000_VCOCLOCK_MAX)
				continue;

			divisor = fl2000_pll_get_divisor(clock_mil, vco_clk, &min_ppm_err);
			if (divisor == 0)
				continue;

			pll->prescaler = prescaler;
			pll->multiplier = multiplier;
			pll->divisor = divisor;
			if (vco_clk < 125000000)
				pll->function = 0;
			else if (vco_clk < 250000000)
				pll->function = 1;
			else if (vco_clk < 500000000)
				pll->function = 2;
			else
				pll->function = 3;
			*clock_calculated = vco_clk / divisor;
		}

	return min_ppm_err;
}

static int fl2000_mode_calc(const struct drm_display_mode *mode,
			    struct drm_display_mode *adjusted_mode, struct fl2000_pll *pll)
{
	u64 ppm_err;
	u32 clock_calculated;
	u64 clock_mil_adjusted;
	const u64 clock_mil = (u64)mode->clock * 1000 * FL2000_PLL_PRECISION;
	const int max_h_adjustment = 10;

	if (mode->clock * 1000 > FL2000_MAX_PIXCLOCK)
		return -EINVAL;

	/*
	 * Try to match pixel clock slightly adjusting htotal value, sequence is:
	 * 0, -1, 1, -2, 2, -3, 3, -4, 4, -5, 5, ...
	 * Here, 's' is used for sign, 'm' is used for modulo, and 'd' is the adjustment value.
	 */
	for (int m = 0, s = 0, d = 0; m <= max_h_adjustment * 2; m++, s = -s, d += m * s) {
		/*
		 * Maximum pixel clock 1GHz, or 10^9Hz. Multiply by 10^6 we get 10^15Hz. Assume
		 * maximum htotal is 10000 pix (no way) we get 10^19 max value and using u64 which
		 * is 1.8*10^19 no overflow can occur.
		 */
		clock_mil_adjusted = div_u64(clock_mil * (mode->htotal + d), mode->htotal);

		/* To keep precision use clock multiplied by 10^6 */
		ppm_err = fl2000_pll_calc(clock_mil_adjusted, pll, &clock_calculated);

		/* Stop searching as soon as the first valid option found */
		if (ppm_err < FL2000_PPM_ERR_MAX) {
			if (adjusted_mode) {
				drm_mode_copy(adjusted_mode, mode);
				adjusted_mode->htotal += d;
				adjusted_mode->clock = clock_calculated / 1000;
			}

			return 0;
		}
	}

	/* Cannot find a PLL configuration that satisfies the requirements */
	return -EINVAL;
}

static enum drm_mode_status fl2000_crtc_mode_valid(struct drm_crtc *crtc,
						   const struct drm_display_mode *mode)
{
	struct fl2000_drm_if *drm_if = to_fl2000_drm_if(crtc->dev);
	struct drm_display_mode adjusted_mode;
	struct fl2000_pll pll;

	/* Get PLL configuration and check if mode adjustments are needed */
	if (fl2000_mode_calc(mode, &adjusted_mode, &pll))
		return MODE_BAD;

	if (fl2000_get_bytes_pix(drm_if->usb_dev->speed, adjusted_mode.clock) == 0)
		return MODE_BAD;

	return MODE_OK;
}

/*
 * Primary plane
 */

static const u64 fl2000_plane_format_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

static int fl2000_plane_atomic_check(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *new_crtc_state = NULL;

	if (new_plane_state->crtc)
		new_crtc_state = drm_atomic_get_new_crtc_state(state, new_plane_state->crtc);

	return drm_atomic_helper_check_plane_state(new_plane_state, new_crtc_state,
						   DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
						   false, false);
}

/*
 * The whole visible frame is converted on every update with damage: the
 * stream rotates through several buffers, so a partial update would need a
 * persistent copy of the frame to patch.
 */
static void fl2000_plane_atomic_update(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct drm_plane_state *old_plane_state = drm_atomic_get_old_plane_state(state, plane);
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow_plane_state = to_drm_shadow_plane_state(plane_state);
	struct fl2000_drm_if *drm_if = to_fl2000_drm_if(plane->dev);
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_rect damage, clip;
	int idx;

	if (!fb)
		return; /* plane is disabled */

	if (!drm_atomic_helper_damage_merged(old_plane_state, plane_state, &damage))
		return;

	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		return;

	if (drm_dev_enter(plane->dev, &idx)) {
		/* The plane covers the whole CRTC (no scaling or positioning) */
		drm_rect_fp_to_int(&clip, &plane_state->src);
		fl2000_stream_compress(drm_if->stream, &shadow_plane_state->data[0], fb, &clip,
				       &shadow_plane_state->fmtcnv_state);
		drm_dev_exit(idx);
	}

	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
}

static const struct drm_plane_helper_funcs fl2000_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = fl2000_plane_atomic_check,
	.atomic_update = fl2000_plane_atomic_update,
};

static const struct drm_plane_funcs fl2000_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

/*
 * CRTC
 */

static int fl2000_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	struct fl2000_drm_if *drm_if = to_fl2000_drm_if(crtc->dev);
	struct fl2000_pll pll;
	int ret;

	ret = drm_crtc_helper_atomic_check(crtc, state);
	if (ret)
		return ret;

	if (!crtc_state->enable || !drm_atomic_crtc_needs_modeset(crtc_state))
		return 0;

	/* PLL configuration, with htotal and clock adjusted to what the PLL can produce */
	ret = fl2000_mode_calc(&crtc_state->mode, &crtc_state->adjusted_mode, &pll);
	if (ret)
		return ret;

	/* The USB link must be able to carry the adjusted pixel clock */
	if (!fl2000_get_bytes_pix(drm_if->usb_dev->speed, crtc_state->adjusted_mode.clock))
		return -EINVAL;

	return 0;
}

/* Called before the bridge's mode_set: the FL2000 must be clocked first */
static void fl2000_crtc_mode_set_nofb(struct drm_crtc *crtc)
{
	struct drm_device *drm = crtc->dev;
	struct fl2000_drm_if *drm_if = to_fl2000_drm_if(drm);
	struct usb_device *usb_dev = drm_if->usb_dev;
	const struct drm_display_mode *mode = &crtc->state->mode;
	const struct drm_display_mode *adjusted_mode = &crtc->state->adjusted_mode;
	struct fl2000_timings timings;
	struct fl2000_pll pll;
	unsigned int bytes_pix;

	/*
	 * Same computation as in atomic_check, which already validated the mode
	 * and stored the adjusted htotal and clock in adjusted_mode.
	 */
	if (WARN_ON(fl2000_mode_calc(mode, NULL, &pll)))
		return;
	bytes_pix = fl2000_get_bytes_pix(usb_dev->speed, adjusted_mode->clock);

	drm_dbg_kms(drm, "Mode requested:  " DRM_MODE_FMT "\n", DRM_MODE_ARG(mode));
	drm_dbg_kms(drm, "Mode configured: " DRM_MODE_FMT "\n", DRM_MODE_ARG(adjusted_mode));

	/* Prepare timing configuration */
	timings.hactive = adjusted_mode->hdisplay;
	timings.htotal = adjusted_mode->htotal;
	timings.hsync_width = adjusted_mode->hsync_end - adjusted_mode->hsync_start;
	timings.hstart = adjusted_mode->htotal - adjusted_mode->hsync_start + 1;
	timings.vactive = adjusted_mode->vdisplay;
	timings.vtotal = adjusted_mode->vtotal;
	timings.vsync_width = adjusted_mode->vsync_end - adjusted_mode->vsync_start;
	timings.vstart = adjusted_mode->vtotal - adjusted_mode->vsync_start + 1;

	/* Set PLL settings */
	fl2000_set_pll(usb_dev, &pll);

	/* Reset FL2000 and confirm PLL settings */
	fl2000_reset(usb_dev);

	/* Set timings settings */
	fl2000_set_timings(usb_dev, &timings);

	/* Pixel format according to number of bytes per pixel */
	fl2000_set_pixfmt(usb_dev, bytes_pix);

	/* Configure frame transfers */
	fl2000_set_transfers(usb_dev);

	/* Enable interrupts */
	fl2000_enable_interrupts(usb_dev);

	fl2000_afe_magic(usb_dev);

	fl2000_stream_mode_set(drm_if->stream, mode->hdisplay * mode->vdisplay, bytes_pix);
}

static void fl2000_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct fl2000_drm_if *drm_if = to_fl2000_drm_if(crtc->dev);

	fl2000_stream_enable(drm_if->stream);

	drm_crtc_vblank_atomic_enable(crtc, state);
}

static void fl2000_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct fl2000_drm_if *drm_if = to_fl2000_drm_if(crtc->dev);

	fl2000_stream_disable(drm_if->stream);

	drm_crtc_vblank_atomic_disable(crtc, state);
}

static const struct drm_crtc_helper_funcs fl2000_crtc_helper_funcs = {
	.mode_valid = fl2000_crtc_mode_valid,
	.atomic_check = fl2000_crtc_atomic_check,
	.mode_set_nofb = fl2000_crtc_mode_set_nofb,
	.atomic_flush = drm_crtc_vblank_atomic_flush,
	.atomic_enable = fl2000_crtc_atomic_enable,
	.atomic_disable = fl2000_crtc_atomic_disable,
};

static const struct drm_crtc_funcs fl2000_crtc_funcs = {
	.atomic_create_state = drm_atomic_helper_crtc_create_state,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	/*
	 * The FL2000 has no vblank interrupt: vblank events come from a timer
	 * running at the mode's refresh rate.
	 */
	DRM_CRTC_VBLANK_TIMER_FUNCS,
};

/*
 * Encoder: the FL2000 is programmed from the CRTC, the IT66121 bridge hangs
 * off this encoder.
 */

static const struct drm_encoder_funcs fl2000_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static void fl2000_drm_if_release(struct device *dev, void *res)
{
	struct fl2000_drm_if *drm_if = *(struct fl2000_drm_if **)res;
	struct drm_device *drm = &drm_if->drm;
	struct usb_device *usb_dev = drm_if->usb_dev;

	dev_dbg(dev, "Unbinding FL2000 master\n");

	/*
	 * Block userspace access and shut down the display pipeline while all
	 * the components are still bound.
	 */
	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);

	/*
	 * Detach bridge component. This release function runs with the global
	 * component mutex held (component_master_del() -> master unbind ->
	 * devres_release()), so it must never call component_del() - that would
	 * self-deadlock. The IT66121 is destroyed by a usb_device devres instead.
	 */
	component_unbind_all(dev, drm);

	/* Stop streaming interface */
	fl2000_stream_destroy(usb_dev);

	/* Stop interrupts interface */
	fl2000_intr_destroy(usb_dev);

	/*
	 * Final drm_dev_put() is handled by devm from devm_drm_dev_alloc() when
	 * the master device goes away.
	 */
}

int fl2000_drm_bind(struct device *master)
{
	int ret = 0;
	struct usb_device *usb_dev = to_usb_device(master->parent);
	struct fl2000_drm_if *drm_if;
	struct fl2000_drm_if **holder;
	struct drm_device *drm;
	struct drm_mode_config *mode_config;

	dev_dbg(master, "Binding FL2000 master\n");

	drm_if = devm_drm_dev_alloc(master, &fl2000_drm_driver, struct fl2000_drm_if, drm);
	if (IS_ERR(drm_if)) {
		dev_err(master, "Cannot allocate DRM structure (%pe)\n", drm_if);
		return PTR_ERR(drm_if);
	}
	drm = &drm_if->drm;
	drm_if->usb_dev = usb_dev;

	/*
	 * Devres that fl2000_drm_unbind() releases to tear everything down,
	 * registered once binding succeeded. Note that 'drm_if' itself is
	 * managed by devm_drm_dev_alloc().
	 */
	holder = devres_alloc(fl2000_drm_if_release, sizeof(*holder), GFP_KERNEL);
	if (!holder)
		return -ENOMEM;

	ret = drmm_mode_config_init(drm);
	if (ret) {
		drm_err(drm, "Cannot initialize DRM mode (%d)\n", ret);
		goto err_free_holder;
	}

	mode_config = &drm->mode_config;
	mode_config->funcs = &fl2000_mode_config_funcs;
	mode_config->min_width = 1;
	mode_config->max_width = FL2000_MAX_WIDTH;
	mode_config->min_height = 1;
	mode_config->max_height = FL2000_MAX_HEIGHT;

	ret = drm_universal_plane_init(drm, &drm_if->plane, 0, &fl2000_plane_funcs,
				       fl2000_pixel_formats, ARRAY_SIZE(fl2000_pixel_formats),
				       fl2000_plane_format_modifiers, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret) {
		drm_err(drm, "Cannot initialize plane (%d)\n", ret);
		goto err_free_holder;
	}
	drm_plane_helper_add(&drm_if->plane, &fl2000_plane_helper_funcs);

	/*
	 * Enable the damage-clips property the update path relies on
	 * (drm_atomic_helper_damage_merged()).
	 */
	drm_plane_enable_fb_damage_clips(&drm_if->plane);

	ret = drm_crtc_init_with_planes(drm, &drm_if->crtc, &drm_if->plane, NULL,
					&fl2000_crtc_funcs, NULL);
	if (ret) {
		drm_err(drm, "Cannot initialize CRTC (%d)\n", ret);
		goto err_free_holder;
	}
	drm_crtc_helper_add(&drm_if->crtc, &fl2000_crtc_helper_funcs);

	ret = drm_encoder_init(drm, &drm_if->encoder, &fl2000_encoder_funcs,
			       DRM_MODE_ENCODER_NONE, NULL);
	if (ret) {
		drm_err(drm, "Cannot initialize encoder (%d)\n", ret);
		goto err_free_holder;
	}
	drm_if->encoder.possible_crtcs = drm_crtc_mask(&drm_if->crtc);

	/* Start streaming interface */
	drm_if->stream = fl2000_stream_create(usb_dev);
	if (IS_ERR(drm_if->stream)) {
		ret = PTR_ERR(drm_if->stream);
		drm_err(drm, "Cannot create stream (%d)\n", ret);
		goto err_free_holder;
	}

	/* Start interrupts interface */
	drm_if->intr = fl2000_intr_create(usb_dev, drm);
	if (IS_ERR(drm_if->intr)) {
		ret = PTR_ERR(drm_if->intr);
		drm_err(drm, "Cannot start interrupt polling (%d)\n", ret);
		goto err_destroy_stream;
	}

	/* Attach bridge */
	ret = component_bind_all(master, drm);
	if (ret) {
		drm_err(drm, "Cannot attach bridge (%d)\n", ret);
		goto err_destroy_intr;
	}

	drm_mode_config_reset(drm);

	ret = drm_vblank_init(drm, drm->mode_config.num_crtc);
	if (ret) {
		drm_err(drm, "Failed to initialize %d VBLANK(s) (%d)\n",
			drm->mode_config.num_crtc, ret);
		goto err_unbind;
	}

	ret = drm_dev_register(drm, 0);
	if (ret) {
		drm_err(drm, "Cannot register DRM device (%d)\n", ret);
		goto err_unbind;
	}

	*holder = drm_if;
	devres_add(master, holder);

	fl2000_reset(usb_dev);
	fl2000_usb_magic(usb_dev);

	/*
	 * fbdev emulation, so the console can use the display. Last, because it
	 * may set a mode right away and the reset above would undo it.
	 */
	drm_client_setup(drm, NULL);

	return 0;

err_unbind:
	component_unbind_all(master, drm);
err_destroy_intr:
	fl2000_intr_destroy(usb_dev);
err_destroy_stream:
	fl2000_stream_destroy(usb_dev);
err_free_holder:
	devres_free(holder);
	return ret;
}

void fl2000_drm_unbind(struct device *master)
{
	devres_release(master, fl2000_drm_if_release, NULL, NULL);
}
