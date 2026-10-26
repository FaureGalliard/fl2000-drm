// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2019, Artem Mygaiev
 */

#include <linux/bitfield.h>
#include <linux/delay.h>

#include "fl2000.h"

#define CONTROL_MSG_READ  64
#define CONTROL_MSG_WRITE 65

#define FL2000_HW_RST_MDELAY 10

static bool fl2000_reg_precious(struct device *dev, unsigned int reg)
{
	return FL2000_REG_PRECIOUS(reg);
}

static bool fl2000_reg_volatile(struct device *dev, unsigned int reg)
{
	return FL2000_REG_VOLATILE(reg);
}

/* Protected by internal regmap mutex */
static int fl2000_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	struct usb_device *usb_dev = context;
	__le32 data;
	int ret;

	ret = usb_control_msg_recv(usb_dev, 0, CONTROL_MSG_READ, USB_DIR_IN | USB_TYPE_VENDOR, 0,
				   reg, &data, sizeof(data), USB_CTRL_GET_TIMEOUT, GFP_KERNEL);
	if (ret)
		return ret;

	*val = le32_to_cpu(data);

	return 0;
}

/* Protected by internal regmap mutex */
static int fl2000_reg_write(void *context, unsigned int reg, unsigned int val)
{
	struct usb_device *usb_dev = context;
	__le32 data = cpu_to_le32(val);

	return usb_control_msg_send(usb_dev, 0, CONTROL_MSG_WRITE, USB_DIR_OUT | USB_TYPE_VENDOR, 0,
				    reg, &data, sizeof(data), USB_CTRL_SET_TIMEOUT, GFP_KERNEL);
}

/*
 * We do not use default values as per documentation because:
 *  a) somehow they differ from real HW
 *  b) on SW reset not all of them are cleared
 */
static const struct regmap_config fl2000_regmap_config = {
	.val_bits = 32,
	.reg_bits = 16,
	.reg_stride = 4,
	.max_register = 0xFFFF,

	.cache_type = REGCACHE_RBTREE,

	.precious_reg = fl2000_reg_precious,
	.volatile_reg = fl2000_reg_volatile,

	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,

	.reg_read = fl2000_reg_read,
	.reg_write = fl2000_reg_write,

	.use_single_read = true,
	.use_single_write = true,
};

int fl2000_set_pll(struct usb_device *usb_dev, struct fl2000_pll *pll)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	u32 val;

	val = FIELD_PREP(FL2000_VGA_PLL_PRESCALER, pll->prescaler) |
	      FIELD_PREP(FL2000_VGA_PLL_MULTIPLIER, pll->multiplier) |
	      FIELD_PREP(FL2000_VGA_PLL_DIVISOR, pll->divisor) |
	      FIELD_PREP(FL2000_VGA_PLL_FUNCTION, pll->function);
	regmap_write(regmap, FL2000_VGA_PLL_REG, val);

	val = FL2000_VGA_CTRL_ACLK_FORCE_PLL_UP | FL2000_VGA_CTRL_ACLK_FORCE_VGA_CONNECT;
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, val, val);

	return 0;
}

int fl2000_set_timings(struct usb_device *usb_dev, struct fl2000_timings *timings)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);

	regmap_write(regmap, FL2000_VGA_HSYNC_REG1,
		     FIELD_PREP(FL2000_VGA_HSYNC1_HACTIVE, timings->hactive) |
		     FIELD_PREP(FL2000_VGA_HSYNC1_HTOTAL, timings->htotal));

	regmap_write(regmap, FL2000_VGA_HSYNC_REG2,
		     FIELD_PREP(FL2000_VGA_HSYNC2_HSYNC_WIDTH, timings->hsync_width) |
		     FIELD_PREP(FL2000_VGA_HSYNC2_HSTART, timings->hstart));

	regmap_write(regmap, FL2000_VGA_VSYNC_REG1,
		     FIELD_PREP(FL2000_VGA_VSYNC1_VACTIVE, timings->vactive) |
		     FIELD_PREP(FL2000_VGA_VSYNC1_VTOTAL, timings->vtotal));

	regmap_write(regmap, FL2000_VGA_VSYNC_REG2,
		     FIELD_PREP(FL2000_VGA_VSYNC2_VSYNC_WIDTH, timings->vsync_width) |
		     FIELD_PREP(FL2000_VGA_VSYNC2_VSTART, timings->vstart) |
		     FIELD_PREP(FL2000_VGA_VSYNC2_START_LATENCY, timings->vstart));

	return 0;
}

int fl2000_set_pixfmt(struct usb_device *usb_dev, u32 bytes_pix)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	u32 mask, val;

	mask = FL2000_VGA_CTRL_PXCLK_DAC_OUTPUT_EN | FL2000_VGA_CTRL_PXCLK_DROP_CNT |
	       FL2000_VGA_CTRL_PXCLK_VGA565_MODE | FL2000_VGA_CTRL_PXCLK_VGA332_MODE |
	       FL2000_VGA_CTRL_PXCLK_VGA555_MODE | FL2000_VGA_CTRL_PXCLK_VGA_COMPRESS |
	       FL2000_VGA_CTRL_PXCLK_CLEAR_WATERMARK;
	val = FL2000_VGA_CTRL_PXCLK_DAC_OUTPUT_EN | FL2000_VGA_CTRL_PXCLK_CLEAR_WATERMARK;
	if (bytes_pix == 2)
		val |= FL2000_VGA_CTRL_PXCLK_VGA565_MODE;
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_PXCLK, mask, val);

	return 0;
}

int fl2000_set_transfers(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	u32 mask;

	mask = FL2000_VGA_CTRL_ACLK_USE_PKT_PENDING | FL2000_VGA_CTRL_ACLK_USE_ZERO_TD |
	       FL2000_VGA_CTRL_ACLK_USE_ZERO_PKT_LEN;
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask,
			  FL2000_VGA_CTRL_ACLK_USE_ZERO_PKT_LEN);

	regmap_write_bits(regmap, FL2000_VGA_ISOCH_REG, FL2000_VGA_ISOCH_MFRAME_CNT, 0);

	return 0;
}

int fl2000_reset(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);

	regmap_write_bits(regmap, FL2000_RST_CTRL_REG, FL2000_RST_CTRL_SW_RESET,
			  FL2000_RST_CTRL_SW_RESET);

	msleep(FL2000_HW_RST_MDELAY);

	return 0;
}

int fl2000_afe_magic(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);

	regmap_write_bits(regmap, FL2000_USB_LPM_REG, FL2000_USB_LPM_MAGIC, FL2000_USB_LPM_MAGIC);

	return 0;
}

int fl2000_usb_magic(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	u32 val;

	val = FL2000_VGA_I2C_SC_MONITOR_DETECT | FL2000_VGA_I2C_SC_EDID_DETECT;
	regmap_write_bits(regmap, FL2000_VGA_I2C_SC_REG, val, val);

	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_3, FL2000_VGA_CTRL3_WAKEUP_CLR_EN, 0);

	val = FL2000_USB_LPM_U1_REJECT | FL2000_USB_LPM_U2_REJECT;
	regmap_write_bits(regmap, FL2000_USB_LPM_REG, val, val);

	regmap_write_bits(regmap, FL2000_USB_CTRL_REG, FL2000_USB_CTRL_WAKE_NRDY, 0);

	return 0;
}

int fl2000_enable_interrupts(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	u32 mask, val;

	val = FL2000_VGA_CTRL_ACLK_VGA_ERR_INT_EN | FL2000_VGA_CTRL_ACLK_LBUF_ERR_INT_EN |
	      FL2000_VGA_CTRL_ACLK_EDID_MON_INT_EN;
	mask = val | FL2000_VGA_CTRL_ACLK_FEEDBACK_INT_EN;
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask, val);

	regmap_write_bits(regmap, FL2000_VGA_CTRL2_REG_ACLK, FL2000_VGA_CTRL2_ACLK_HDMI_INT_EN,
			  FL2000_VGA_CTRL2_ACLK_HDMI_INT_EN);

	return 0;
}

int fl2000_check_interrupt(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	unsigned int status;
	int ret;
	int sink_event = 0;
	u32 mask;

	/* Process interrupt */
	ret = regmap_read(regmap, FL2000_VGA_STATUS_REG, &status);
	if (ret)
		return 0; /* Treat as "no event": there is no caller to report to */

	if (status & (FL2000_VGA_STATUS_HDMI_EVENT | FL2000_VGA_STATUS_MONITOR_EVENT |
		      FL2000_VGA_STATUS_EDID_EVENT))
		sink_event = 1;

	/* LBUF issues are recoverable */
	mask = status & (FL2000_VGA_STATUS_LBUF_OVERFLOW | FL2000_VGA_STATUS_LBUF_UNDERFLOW);
	regmap_write_bits(regmap, FL2000_VGA_STATUS_REG, mask, status);

	/* lbuf_halt and vga_error are not handled: no recovery sequence is known */

	return sink_event;
}

int fl2000_i2c_dword(struct usb_device *usb_dev, bool read, u16 addr, u8 offset, u32 *data)
{
	int ret;
	unsigned int val;
	u32 mask;
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);

	if (!read) {
		ret = regmap_write(regmap, FL2000_VGA_I2C_WR_REG, *data);
		if (ret)
			return -EIO;
	}

	/*
	 * The detection bits always read back as 0, so they need to be set again on every write.
	 * It is not clear whether the monitor detection circuit is needed at all for the HDMI use
	 * case.
	 */
	mask = FL2000_VGA_I2C_SC_MONITOR_DETECT | FL2000_VGA_I2C_SC_EDID_DETECT |
	       FL2000_VGA_I2C_SC_STATUS | FL2000_VGA_I2C_SC_ADDR | FL2000_VGA_I2C_SC_CMD |
	       FL2000_VGA_I2C_SC_OFFSET | FL2000_VGA_I2C_SC_DONE;
	val = FL2000_VGA_I2C_SC_MONITOR_DETECT | FL2000_VGA_I2C_SC_EDID_DETECT |
	      FIELD_PREP(FL2000_VGA_I2C_SC_ADDR, addr) |
	      FIELD_PREP(FL2000_VGA_I2C_SC_CMD, read) |
	      FIELD_PREP(FL2000_VGA_I2C_SC_OFFSET, offset);

	ret = regmap_write_bits(regmap, FL2000_VGA_I2C_SC_REG, mask, val);
	if (ret)
		return -EIO;

	ret = regmap_read_poll_timeout(regmap, FL2000_VGA_I2C_SC_REG, val,
				       val & FL2000_VGA_I2C_SC_DONE, I2C_RDWR_INTERVAL,
				       I2C_RDWR_TIMEOUT);
	/*
	 * This shouldn't normally happen: there's internal 256ms HW timeout on I2C operations and
	 * USB must be always available so no I/O errors. But if it happens we are probably facing
	 * an irreversible HW issue.
	 */
	if (ret || FIELD_GET(FL2000_VGA_I2C_SC_STATUS, val))
		return -EIO;

	if (read) {
		ret = regmap_read(regmap, FL2000_VGA_I2C_RD_REG, data);
		if (ret)
			return -EIO;
	}

	return 0;
}

struct regmap *fl2000_regmap_init(struct usb_device *usb_dev)
{
	struct regmap *regmap;

	regmap = devm_regmap_init(&usb_dev->dev, NULL, usb_dev, &fl2000_regmap_config);
	if (IS_ERR(regmap))
		dev_err(&usb_dev->dev, "Registers map failed (%pe)\n", regmap);

	return regmap;
}
