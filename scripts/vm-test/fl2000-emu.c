// SPDX-License-Identifier: GPL-2.0
/*
 * A fake FL2000DX + IT66121 for testing the driver in a VM, on raw_gadget and
 * dummy_hcd: registers over vendor control transfers, the I2C engine (IT66121
 * registers at 0x4c, EDID at 0x50), bulk frames on EP1 OUT and the interrupt
 * endpoint EP3 IN.
 *
 *   fl2000-emu [--high-speed] [--no-it66121] [--i2c-fail N] [--edid 0|1]
 *              [--hdmi 0|1] [--vga 0|1] [--ctl fifo] [--log file]
 *
 * Commands on the fifo: hdmi 0|1, vga 0|1, dump <file>, stats, quit.
 * Unplug is killing the process.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/types.h>
#include <linux/usb/ch9.h>

#define UDC_NAME_LENGTH_MAX 128
struct usb_raw_init {
	__u8 driver_name[UDC_NAME_LENGTH_MAX];
	__u8 device_name[UDC_NAME_LENGTH_MAX];
	__u8 speed;
};
enum usb_raw_event_type {
	USB_RAW_EVENT_INVALID = 0,
	USB_RAW_EVENT_CONNECT = 1,
	USB_RAW_EVENT_CONTROL = 2,
	USB_RAW_EVENT_SUSPEND = 3,
	USB_RAW_EVENT_RESUME = 4,
	USB_RAW_EVENT_RESET = 5,
	USB_RAW_EVENT_DISCONNECT = 6,
};
struct usb_raw_event {
	__u32 type;
	__u32 length;
	__u8 data[];
};
struct usb_raw_ep_io {
	__u16 ep;
	__u16 flags;
	__u32 length;
	__u8 data[];
};
#define USB_RAW_IOCTL_INIT _IOW('U', 0, struct usb_raw_init)
#define USB_RAW_IOCTL_RUN _IO('U', 1)
#define USB_RAW_IOCTL_EVENT_FETCH _IOR('U', 2, struct usb_raw_event)
#define USB_RAW_IOCTL_EP0_WRITE _IOW('U', 3, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP0_READ _IOWR('U', 4, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP_ENABLE _IOW('U', 5, struct usb_endpoint_descriptor)
#define USB_RAW_IOCTL_EP_DISABLE _IOW('U', 6, __u32)
#define USB_RAW_IOCTL_EP_WRITE _IOW('U', 7, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP_READ _IOWR('U', 8, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_CONFIGURE _IO('U', 9)
#define USB_RAW_IOCTL_VBUS_DRAW _IOW('U', 10, __u32)
#define USB_RAW_IOCTL_EP0_STALL _IO('U', 12)

#define R_STATUS 0x8000
#define R_I2C_SC 0x8020
#define R_I2C_RD 0x8024
#define R_I2C_WR 0x8028
#define R_ACLK 0x803c
#define R_RST 0x8048
#define R_CTRL2 0x8064
#define ACLK_EDID_MON_INT BIT(2)
#define ACLK_FORCE_VGA BIT(25)
#define CTRL2_HDMI_INT BIT(18)
#define BIT(n) (1u << (n))
#define ST_HDMI_EVENT BIT(26)
#define ST_HDMI_STATUS BIT(27)
#define ST_EDID_STATUS BIT(28)
#define ST_MONITOR_STATUS BIT(29)
#define ST_MONITOR_EVENT BIT(30)
#define ST_EDID_EVENT BIT(31)
#define SC_DONE BIT(31)

static const uint8_t edid_1080p[256] = {
	0x00,0xff,0xff,0xff,0xff,0xff,0xff,0x00,0x5a,0x63,0x40,0xe9,0x88,0x88,0x18,0x30,
	0x14,0x23,0x01,0x03,0x80,0x35,0x1d,0x78,0xea,0x31,0x35,0xa5,0x55,0x4e,0xa1,0x26,
	0x0c,0x50,0x54,0xbf,0xef,0x80,0x71,0x4f,0x81,0x40,0x81,0x80,0x81,0xc0,0x81,0x00,
	0x95,0x00,0xb3,0x00,0x31,0x40,0x02,0x3a,0x80,0x18,0x71,0x38,0x2d,0x40,0x58,0x2c,
	0x45,0x00,0x0f,0x28,0x21,0x00,0x00,0x1e,0x00,0x00,0x00,0xfd,0x00,0x30,0x64,0x1e,
	0x78,0x1e,0x00,0x0a,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xfc,0x00,0x56,
	0x41,0x32,0x34,0x30,0x2d,0x46,0x48,0x44,0x0a,0x20,0x20,0x20,0x00,0x00,0x00,0xff,
	0x00,0x58,0x4e,0x45,0x32,0x35,0x32,0x30,0x32,0x30,0x34,0x37,0x30,0x0a,0x01,0x84,
	0x02,0x03,0x21,0xb2,0x47,0x01,0x03,0x04,0x90,0x12,0x13,0x9f,0x83,0x01,0x00,0x00,
	0x67,0x03,0x0c,0x00,0x10,0x00,0x38,0x3c,0x68,0x1a,0x00,0x00,0x01,0x01,0x30,0x64,
	0xe6,0x2a,0x44,0x80,0xa0,0x70,0x38,0x27,0x40,0x30,0x20,0x35,0x00,0x00,0x00,0x00,
	0x00,0x00,0x1a,0x06,0x5a,0x80,0xa0,0x70,0x38,0x1c,0x40,0x30,0x20,0x35,0x00,0x00,
	0x00,0x00,0x00,0x00,0x1a,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xaa,
};

static int fd;
static bool super_speed = true, has_it66121 = true, has_edid = true;
static volatile bool hdmi = true, vga;
static int i2c_fail;
static int udc_num;
static FILE *logf_;
static uint32_t regs[0x10000 / 4];
static uint8_t it[0x200];
static uint32_t st_events;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t irq_cond = PTHREAD_COND_INITIALIZER;
static int irq_pending;

static volatile int bulk_ep = -1, int_ep = -1;
static volatile bool bulk_busy, int_busy;
static pthread_t bulk_thread, int_thread;

static volatile unsigned long frames, bytes, last_frame_len, set_ifaces, resets;
static uint8_t *frame, *last_frame;
#define FRAME_MAX (48u << 20)

static void logmsg(const char *fmt, ...)
{
	va_list ap;

	if (!logf_)
		return;
	va_start(ap, fmt);
	vfprintf(logf_, fmt, ap);
	va_end(ap);
	fflush(logf_);
}

static uint8_t *it_reg(unsigned int r)
{
	if (r < 0x30)
		return &it[r];
	return &it[(it[0x0f] & 1 ? 0x100 : 0) + r];
}

static void it_reset(void)
{
	memset(it, 0, sizeof(it));
	it[0x00] = 0x54;
	it[0x01] = 0x49;
	it[0x02] = 0x12;
	it[0x03] = 0x16;
	it[0x09] = 0xff;	/* everything masked */
	it[0x16] = 0x80;	/* DDC done */
}

static void it_sync(void)
{
	if (hdmi)
		it[0x0e] |= 0x40;
	else
		it[0x0e] &= ~0x40;
	it[0x16] = 0x80;
}

static void i2c_op(uint32_t sc)
{
	unsigned int addr = sc & 0x7f, offset = (sc >> 8) & 0xff, read = sc & BIT(7);
	unsigned int status = 0;
	uint32_t data = 0;

	if (i2c_fail > 0) {
		i2c_fail--;
		status = 0xf;
	} else if (addr == 0x4c && has_it66121) {
		it_sync();
		if (read) {
			for (int i = 0; i < 4; i++)
				data |= (uint32_t)*it_reg(offset + i) << (i * 8);
			regs[R_I2C_RD / 4] = data;
		} else {
			data = regs[R_I2C_WR / 4];
			for (int i = 0; i < 4; i++) {
				unsigned int r = offset + i;
				uint8_t v = data >> (i * 8);

				if (r == 0x0c) {	/* INT_CLEAR_1 */
					it[0x06] &= ~v;
				} else if (r == 0x0e) {	/* SYS_STATUS: only the clear bit */
					if (v & 1 && !(it[0x06] & ~it[0x09]))
						it[0x0e] &= ~0x80;
				} else if (r == 0x16 || r == 0x17) {
					/* status and FIFO */
				} else if (r < 4) {
					/* IDs */
				} else {
					*it_reg(r) = v;
				}
			}
		}
	} else if (addr == 0x50 && has_edid && (hdmi || vga)) {
		if (read) {
			for (int i = 0; i < 4; i++)
				data |= (uint32_t)edid_1080p[(offset + i) & 0xff] << (i * 8);
			regs[R_I2C_RD / 4] = data;
		}
	} else {
		status = 0xf;
	}

	/* The detect bits read as 0 */
	regs[R_I2C_SC / 4] = (sc & 0x0000ffff) | (status << 24) | SC_DONE;
}

static uint32_t reg_read(unsigned int r)
{
	uint32_t v = regs[(r / 4) & 0x3fff];

	if (r == R_STATUS) {
		v = ST_HDMI_STATUS | st_events;
		if (hdmi || vga || (regs[R_ACLK / 4] & ACLK_FORCE_VGA))
			v |= ST_MONITOR_STATUS;
		if (has_edid && (hdmi || vga))
			v |= ST_EDID_STATUS;
		v |= 0xc1;
		st_events = 0;
	}
	return v;
}

static void reg_write(unsigned int r, uint32_t v)
{
	if (r == R_STATUS)
		return;
	if (r == R_I2C_SC) {
		if (!(v & SC_DONE))
			i2c_op(v);
		else
			regs[R_I2C_SC / 4] = (regs[R_I2C_SC / 4] & 0xffff) | (regs[R_I2C_SC / 4] & (0xf << 24)) | SC_DONE;
		return;
	}
	if (r == R_RST) {
		/* The software reset bit clears itself */
		regs[r / 4] = v & ~BIT(15);
		return;
	}
	regs[(r / 4) & 0x3fff] = v;
}

static void raise_irq(void)
{
	irq_pending++;
	pthread_cond_signal(&irq_cond);
}

/* Called with the lock held */
static void sink_change(bool is_hdmi, bool on)
{
	if (is_hdmi) {
		hdmi = on;
		it_sync();
		it[0x06] |= 1;
		if (!(it[0x09] & 1))
			it[0x0e] |= 0x80;
		if ((it[0x0e] & 0x80) && (regs[R_CTRL2 / 4] & CTRL2_HDMI_INT)) {
			st_events |= ST_HDMI_EVENT;
			raise_irq();
		}
	} else {
		vga = on;
	}
	if (regs[R_ACLK / 4] & ACLK_EDID_MON_INT) {
		st_events |= ST_MONITOR_EVENT | ST_EDID_EVENT;
		raise_irq();
	}
}

/* ---------------------------------------------------------------- USB */

#define BCD(x) (x)
static struct usb_device_descriptor dev_desc = {
	.bLength = sizeof(dev_desc),
	.bDescriptorType = USB_DT_DEVICE,
	.bcdUSB = 0x0300,
	.bDeviceClass = 0xef,
	.bDeviceSubClass = 2,
	.bDeviceProtocol = 1,
	.bMaxPacketSize0 = 9,
	.idVendor = 0x1d5c,
	.idProduct = 0x2000,
	.bcdDevice = 0x0200,
	.bNumConfigurations = 1,
};

static struct usb_endpoint_descriptor bulk_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 1,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
};
static struct usb_endpoint_descriptor int_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN | 3,
	.bmAttributes = USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize = 8,
	.bInterval = 4,
};

static int build_config(uint8_t *buf, uint8_t type)
{
	struct usb_config_descriptor *cfg = (void *)buf;
	uint8_t *p = buf + sizeof(*cfg);
	struct usb_interface_descriptor ifd = {
		.bLength = sizeof(ifd),
		.bDescriptorType = USB_DT_INTERFACE,
		.bInterfaceClass = 0x10,
	};
	struct usb_ss_ep_comp_descriptor comp = {
		.bLength = sizeof(comp),
		.bDescriptorType = USB_DT_SS_ENDPOINT_COMP,
	};
	bool ss = super_speed;

	bulk_desc.wMaxPacketSize = ss ? 1024 : 512;

#define ADD(d) do { memcpy(p, &(d), (d).bLength); p += (d).bLength; } while (0)
	ifd.bInterfaceNumber = 0;
	ifd.bAlternateSetting = 0;
	ifd.bNumEndpoints = 0;
	ADD(ifd);
	ifd.bAlternateSetting = 1;
	ifd.bNumEndpoints = 1;
	ADD(ifd);
	ADD(bulk_desc);
	if (ss)
		ADD(comp);
	ifd.bInterfaceNumber = 1;
	ifd.bAlternateSetting = 0;
	ifd.bNumEndpoints = 0;
	ADD(ifd);
	ifd.bInterfaceNumber = 2;
	ifd.bNumEndpoints = 1;
	ADD(ifd);
	ADD(int_desc);
	if (ss) {
		comp.wBytesPerInterval = 8;
		ADD(comp);
	}

	cfg->bLength = sizeof(*cfg);
	cfg->bDescriptorType = type;
	cfg->wTotalLength = p - buf;
	cfg->bNumInterfaces = 3;
	cfg->bConfigurationValue = 1;
	cfg->bmAttributes = 0x80;
	cfg->bMaxPower = 50;
	return p - buf;
}

static struct {
	struct usb_raw_ep_io io;
	uint8_t data[8192];
} ep0;

static void ep0_in(const void *data, int len, int want)
{
	ep0.io.ep = 0;
	ep0.io.flags = 0;
	ep0.io.length = len < want ? len : want;
	memcpy(ep0.data, data, ep0.io.length);
	if (ioctl(fd, USB_RAW_IOCTL_EP0_WRITE, &ep0) < 0)
		logmsg("ep0 write: %s\n", strerror(errno));
}

static int ep0_out(int len)
{
	ep0.io.ep = 0;
	ep0.io.flags = 0;
	ep0.io.length = len;
	return ioctl(fd, USB_RAW_IOCTL_EP0_READ, &ep0);
}

static void ep0_stall(void)
{
	ioctl(fd, USB_RAW_IOCTL_EP0_STALL, 0);
}

static void on_sig(int s)
{
	(void)s;
}

/* Stop the reader, which sits in a blocking read, and disable the endpoint */
static void ep_stop(volatile int *ep, volatile bool *busy, pthread_t th)
{
	int h = *ep;

	if (h < 0)
		return;
	*ep = -1;
	for (int i = 0; i < 2000 && *busy; i++) {
		pthread_kill(th, SIGUSR1);
		usleep(500);
		pthread_mutex_lock(&lock);
		pthread_cond_broadcast(&irq_cond);
		pthread_mutex_unlock(&lock);
	}
	ioctl(fd, USB_RAW_IOCTL_EP_DISABLE, h);
}

static void *bulk_reader(void *arg)
{
	static struct {
		struct usb_raw_ep_io io;
		uint8_t data[65536];
	} b;
	size_t pos = 0;

	(void)arg;
	for (;;) {
		int h = bulk_ep, n;

		if (h < 0) {
			usleep(1000);
			continue;
		}
		bulk_busy = true;
		if (bulk_ep < 0) {
			bulk_busy = false;
			continue;
		}
		b.io.ep = h;
		b.io.flags = 0;
		b.io.length = sizeof(b.data);
		n = ioctl(fd, USB_RAW_IOCTL_EP_READ, &b);
		bulk_busy = false;
		if (n < 0) {
			usleep(1000);
			continue;
		}
		if (pos + n <= FRAME_MAX)
			memcpy(frame + pos, b.data, n);
		pos += n;
		bytes += n;
		if (n < (int)sizeof(b.data)) {
			/* A short read is the end of a frame: the driver sends a zero-length URB */
			if (pos) {
				pthread_mutex_lock(&lock);
				memcpy(last_frame, frame, pos < FRAME_MAX ? pos : FRAME_MAX);
				last_frame_len = pos;
				frames++;
				pthread_mutex_unlock(&lock);
			}
			pos = 0;
		}
	}
	return NULL;
}

static void *int_writer(void *arg)
{
	static struct {
		struct usb_raw_ep_io io;
		uint8_t data[8];
	} b;

	(void)arg;
	for (;;) {
		int h;

		pthread_mutex_lock(&lock);
		while (!irq_pending || int_ep < 0)
			pthread_cond_wait(&irq_cond, &lock);
		irq_pending = 0;
		h = int_ep;
		int_busy = true;
		pthread_mutex_unlock(&lock);
		b.io.ep = h;
		b.io.flags = 0;
		b.io.length = 1;
		b.data[0] = 1;
		if (ioctl(fd, USB_RAW_IOCTL_EP_WRITE, &b) < 0)
			logmsg("int write: %s\n", strerror(errno));
		else
			logmsg("interrupt sent\n");
		int_busy = false;
	}
	return NULL;
}

static void handle_control(const struct usb_ctrlrequest *c)
{
	unsigned int len = c->wLength;

	if ((c->bRequestType & USB_TYPE_MASK) == USB_TYPE_VENDOR) {
		unsigned int r = c->wIndex;
		uint32_t v;

		if (c->bRequest == 64 && (c->bRequestType & USB_DIR_IN)) {
			pthread_mutex_lock(&lock);
			v = reg_read(r);
			pthread_mutex_unlock(&lock);
			ep0_in(&v, sizeof(v), len);
			return;
		}
		if (c->bRequest == 65 && !(c->bRequestType & USB_DIR_IN)) {
			if (ep0_out(len) >= 0) {
				memcpy(&v, ep0.data, sizeof(v));
				pthread_mutex_lock(&lock);
				reg_write(r, v);
				pthread_mutex_unlock(&lock);
			}
			return;
		}
		ep0_stall();
		return;
	}

	if ((c->bRequestType & USB_TYPE_MASK) != USB_TYPE_STANDARD) {
		ep0_stall();
		return;
	}

	switch (c->bRequest) {
	case USB_REQ_GET_DESCRIPTOR: {
		uint8_t type = c->wValue >> 8;
		static uint8_t buf[4096];

		switch (type) {
		case USB_DT_DEVICE:
			ep0_in(&dev_desc, sizeof(dev_desc), len);
			return;
		case USB_DT_CONFIG:
			ep0_in(buf, build_config(buf, USB_DT_CONFIG), len);
			return;
		case USB_DT_BOS: {
			static const uint8_t bos[] = {
				5, USB_DT_BOS, 22, 0, 2,
				7, USB_DT_DEVICE_CAPABILITY, 2, 0x02, 0, 0, 0,
				10, USB_DT_DEVICE_CAPABILITY, 3, 0, 0x0e, 0, 3, 10, 0xff, 0x07,
			};
			ep0_in(bos, sizeof(bos), len);
			return;
		}
		case USB_DT_STRING: {
			static const uint8_t lang[] = { 4, USB_DT_STRING, 0x09, 0x04 };

			if ((c->wValue & 0xff) == 0) {
				ep0_in(lang, sizeof(lang), len);
				return;
			}
			break;
		}
		default:
			break;
		}
		ep0_stall();
		return;
	}
	case USB_REQ_SET_CONFIGURATION: {
		__u32 power = 100;

		ioctl(fd, USB_RAW_IOCTL_VBUS_DRAW, power);
		ioctl(fd, USB_RAW_IOCTL_CONFIGURE, 0);
		ep_stop(&int_ep, &int_busy, int_thread);
		if (c->wValue) {
			int h = ioctl(fd, USB_RAW_IOCTL_EP_ENABLE, &int_desc);

			if (h < 0)
				logmsg("int ep enable: %s\n", strerror(errno));
			pthread_mutex_lock(&lock);
			int_ep = h;
			pthread_cond_broadcast(&irq_cond);
			pthread_mutex_unlock(&lock);
		}
		ep0_out(0);
		return;
	}
	case USB_REQ_SET_INTERFACE:
		set_ifaces++;
		if (c->wIndex == 0) {
			ep_stop(&bulk_ep, &bulk_busy, bulk_thread);
			if (c->wValue == 1) {
				int h = ioctl(fd, USB_RAW_IOCTL_EP_ENABLE, &bulk_desc);

				if (h < 0)
					logmsg("bulk ep enable: %s\n", strerror(errno));
				bulk_ep = h;
			}
		}
		ep0_out(0);
		return;
	case USB_REQ_GET_INTERFACE: {
		uint8_t alt = (c->wIndex == 0 && bulk_ep >= 0) ? 1 : 0;

		ep0_in(&alt, 1, len);
		return;
	}
	case USB_REQ_SET_SEL:
	case USB_REQ_SET_ISOCH_DELAY:
		if (len)
			ep0_out(len);
		else
			ep0_out(0);
		return;
	default:
		ep0_stall();
		return;
	}
}

static void usb_reset_state(void)
{
	ep_stop(&bulk_ep, &bulk_busy, bulk_thread);
	ep_stop(&int_ep, &int_busy, int_thread);
	pthread_mutex_lock(&lock);
	/* A reset of the USB device with the power gone: every register is lost */
	memset(regs, 0, sizeof(regs));
	regs[R_I2C_SC / 4] = SC_DONE;
	it_reset();
	resets++;
	pthread_mutex_unlock(&lock);
}

static void *ctl_thread(void *arg)
{
	const char *path = arg;
	char line[256];

	for (;;) {
		FILE *f = fopen(path, "r");

		if (!f)
			return NULL;
		while (fgets(line, sizeof(line), f)) {
			int v;
			char file[200];

			pthread_mutex_lock(&lock);
			if (sscanf(line, "hdmi %d", &v) == 1) {
				sink_change(true, v);
			} else if (sscanf(line, "vga %d", &v) == 1) {
				sink_change(false, v);
			} else if (sscanf(line, "dump %199s", file) == 1) {
				FILE *o = fopen(file, "w");

				if (o) {
					fwrite(last_frame, 1, last_frame_len, o);
					fclose(o);
				}
				logmsg("dumped %lu bytes\n", last_frame_len);
			} else if (!strncmp(line, "stats", 5)) {
				logmsg("frames=%lu bytes=%lu last=%lu setif=%lu resets=%lu aclk=%08x it09=%02x it06=%02x\n",
				       frames, bytes, last_frame_len, set_ifaces, resets,
				       regs[R_ACLK / 4], it[9], it[6]);
			} else if (!strncmp(line, "quit", 4)) {
				_exit(0);
			}
			pthread_mutex_unlock(&lock);
		}
		fclose(f);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	struct usb_raw_init init = { 0 };
	static struct {
		struct usb_raw_event ev;
		uint8_t data[4096];
	} e;
	struct sigaction sa = { .sa_handler = on_sig };
	const char *ctl = NULL;
	pthread_t th;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--high-speed")) {
			super_speed = false;
		} else if (!strcmp(argv[i], "--no-it66121")) {
			has_it66121 = false;
		} else if (!strcmp(argv[i], "--i2c-fail")) {
			i2c_fail = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--edid")) {
			has_edid = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--hdmi")) {
			hdmi = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--vga")) {
			vga = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--udc")) {
			udc_num = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--ctl")) {
			ctl = argv[++i];
		} else if (!strcmp(argv[i], "--log")) {
			logf_ = fopen(argv[++i], "a");
		}
	}

	frame = malloc(FRAME_MAX);
	last_frame = malloc(FRAME_MAX);
	regs[R_I2C_SC / 4] = SC_DONE;
	it_reset();

	sigaction(SIGUSR1, &sa, NULL);
	sigemptyset(&sa.sa_mask);

	fd = open("/dev/raw-gadget", O_RDWR);
	if (fd < 0) {
		perror("open /dev/raw-gadget");
		return 1;
	}

	if (!super_speed) {
		dev_desc.bcdUSB = 0x0210;
		dev_desc.bMaxPacketSize0 = 64;
	}
	strcpy((char *)init.driver_name, "dummy_udc");
	snprintf((char *)init.device_name, sizeof(init.device_name), "dummy_udc.%d", udc_num);
	init.speed = super_speed ? USB_SPEED_SUPER : USB_SPEED_HIGH;
	if (ioctl(fd, USB_RAW_IOCTL_INIT, &init) < 0) {
		perror("init");
		return 1;
	}

	pthread_create(&bulk_thread, NULL, bulk_reader, NULL);
	pthread_create(&int_thread, NULL, int_writer, NULL);
	if (ctl)
		pthread_create(&th, NULL, ctl_thread, (void *)ctl);

	if (ioctl(fd, USB_RAW_IOCTL_RUN, 0) < 0) {
		perror("run");
		return 1;
	}
	logmsg("running\n");

	for (;;) {
		e.ev.type = 0;
		e.ev.length = sizeof(e.data);
		if (ioctl(fd, USB_RAW_IOCTL_EVENT_FETCH, &e) < 0) {
			if (errno == EINTR)
				continue;
			perror("event fetch");
			return 1;
		}
		switch (e.ev.type) {
		case USB_RAW_EVENT_CONTROL:
			handle_control((void *)e.ev.data);
			break;
		case USB_RAW_EVENT_RESET:
			logmsg("reset\n");
			usb_reset_state();
			break;
		case USB_RAW_EVENT_DISCONNECT:
			logmsg("disconnect\n");
			usb_reset_state();
			break;
		default:
			break;
		}
	}
	return 0;
}
