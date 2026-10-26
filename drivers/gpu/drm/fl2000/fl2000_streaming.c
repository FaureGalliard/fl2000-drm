// SPDX-License-Identifier: GPL-2.0
/*
 * Frame streaming. The pixel data is sent as one bulk transfer per frame on
 * endpoint 1 OUT, after switching the AV control interface (#0) to
 * altsetting 1. The hardware exposes endpoint #1 on more than one interface,
 * which the USB specification only allows across alternate settings.
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include <linux/scatterlist.h>
#include <linux/vmalloc.h>

#include "fl2000.h"

/*
 * Triple buffering:
 *  - one buffer for HDMI rendering
 *  - one buffer for USB transmission
 *  - one buffer for DRM/KMS data copy
 */
#define FL2000_SB_MIN 3
#define FL2000_SB_NUM (FL2000_SB_MIN + 1)

#define FL2000_URB_TIMEOUT 100

struct fl2000_stream_buf {
	struct list_head list;
	struct sg_table sgt;
	struct page **pages;
	unsigned int nr_pages;
	void *vaddr;
};

struct fl2000_stream {
	struct usb_device *usb_dev;
	struct drm_crtc *crtc;
	/* Each buffer journey: render->transmit->wait->... */
	struct list_head render_list;
	struct list_head transmit_list;
	struct list_head wait_list;
	spinlock_t list_lock; /* Protects the lists, taken from URB completion */
	size_t buf_size;
	u32 bytes_pix;
	struct work_struct work;
	struct workqueue_struct *work_queue;
	/* URBs the work may still submit: limits the frames in flight */
	atomic_t credits;
	wait_queue_head_t credits_wq;
	bool enabled;
	struct usb_anchor anchor;
};

static void fl2000_free_sb(struct fl2000_stream_buf *sb)
{
	vunmap(sb->vaddr);

	sg_free_table(&sb->sgt);

	for (int i = 0; i < sb->nr_pages && sb->pages[i]; i++)
		__free_page(sb->pages[i]);

	kfree(sb->pages);

	kfree(sb);
}

static struct fl2000_stream_buf *fl2000_alloc_sb(unsigned int size)
{
	int ret;
	struct fl2000_stream_buf *sb;
	unsigned int nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;

	sb = kzalloc_obj(*sb);
	if (!sb)
		return NULL;

	sb->nr_pages = nr_pages;

	sb->pages = kzalloc_objs(*sb->pages, nr_pages);
	if (!sb->pages)
		goto error;

	for (int i = 0; i < nr_pages; i++) {
		sb->pages[i] = alloc_page(GFP_KERNEL);
		if (!sb->pages[i])
			goto error;
	}

	ret = sg_alloc_table_from_pages(&sb->sgt, sb->pages, nr_pages, 0, size, GFP_KERNEL);
	if (ret != 0)
		goto error;

	sb->vaddr = vmap(sb->pages, nr_pages, VM_MAP, PAGE_KERNEL);
	if (!sb->vaddr)
		goto error;

	INIT_LIST_HEAD(&sb->list);
	memset(sb->vaddr, 0, nr_pages << PAGE_SHIFT);

	return sb;

error:
	fl2000_free_sb(sb);

	return NULL;
}

static void fl2000_stream_put_buffers(struct fl2000_stream *stream)
{
	struct fl2000_stream_buf *cur_sb;
	struct fl2000_stream_buf *temp_sb;

	list_for_each_entry_safe(cur_sb, temp_sb, &stream->render_list, list) {
		list_del(&cur_sb->list);
		fl2000_free_sb(cur_sb);
	}
}

static int fl2000_stream_get_buffers(struct fl2000_stream *stream, unsigned int size)
{
	int ret;
	struct fl2000_stream_buf *cur_sb;

	if (WARN_ON(!list_empty(&stream->render_list)))
		return -EBUSY;

	for (int i = 0; i < FL2000_SB_NUM; i++) {
		cur_sb = fl2000_alloc_sb(size);
		if (!cur_sb) {
			ret = -ENOMEM;
			goto error;
		}

		list_add(&cur_sb->list, &stream->render_list);
	}

	return 0;

error:
	fl2000_stream_put_buffers(stream);
	return ret;
}

static void fl2000_stream_release(struct device *dev, void *res)
{
	struct fl2000_stream *stream = res;

	fl2000_stream_disable(stream);
	destroy_workqueue(stream->work_queue);
	fl2000_stream_put_buffers(stream);
}

static void fl2000_stream_data_completion(struct urb *urb)
{
	struct fl2000_stream_buf *cur_sb = urb->transfer_buffer;
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_stream *stream = urb->context;
	unsigned long flags;

	if (stream) {
		spin_lock_irqsave(&stream->list_lock, flags);
		list_move_tail(&cur_sb->list, &stream->render_list);
		spin_unlock_irqrestore(&stream->list_lock, flags);

		drm_crtc_handle_vblank(stream->crtc);

		/* Return the credit and kick the transmit work */
		atomic_inc(&stream->credits);
		wake_up(&stream->credits_wq);

		fl2000_urb_status(usb_dev, urb->status);
	}

	usb_free_urb(urb);
}

static void fl2000_stream_work(struct work_struct *work)
{
	int ret;
	struct fl2000_stream *stream = container_of(work, struct fl2000_stream, work);
	struct usb_device *usb_dev = stream->usb_dev;
	struct fl2000_stream_buf *cur_sb;
	struct fl2000_stream_buf *last_sb;
	struct urb *data_urb;

	while (READ_ONCE(stream->enabled)) {
		/* Interruptible sleep: an idle stream may wait here indefinitely */
		ret = wait_event_interruptible(stream->credits_wq, !READ_ONCE(stream->enabled) ||
					       atomic_add_unless(&stream->credits, -1, 0));
		if (ret) {
			dev_err(&usb_dev->dev, "Work interrupt error %d\n", ret);
			WRITE_ONCE(stream->enabled, false);
			return;
		}
		if (!READ_ONCE(stream->enabled))
			break;

		spin_lock_irq(&stream->list_lock);

		if (!list_empty(&stream->transmit_list)) {
			/* A freshly rendered frame is ready: just move it in-flight */
			cur_sb = list_first_entry(&stream->transmit_list, struct fl2000_stream_buf,
						  list);
			list_move_tail(&cur_sb->list, &stream->wait_list);
			spin_unlock_irq(&stream->list_lock);
		} else if (!list_empty(&stream->render_list)) {
			/*
			 * No new frame: resend the latest one. Take a free buffer and
			 * copy the last frame into it. The copy is several MB and MUST
			 * run with interrupts enabled, so cur_sb is taken off all lists
			 * (owned) for the duration and re-inserted afterwards. Holding
			 * list_lock (which disables IRQs) across the memcpy starves the
			 * whole system, e.g. amdgpu IRQs -> fence fallback timers.
			 */
			cur_sb = list_first_entry(&stream->render_list, struct fl2000_stream_buf,
						  list);
			if (!list_empty(&stream->wait_list))
				last_sb = list_last_entry(&stream->wait_list,
							  struct fl2000_stream_buf, list);
			else
				last_sb = list_last_entry(&stream->render_list,
							  struct fl2000_stream_buf, list);
			list_del_init(&cur_sb->list);
			spin_unlock_irq(&stream->list_lock);

			/*
			 * For a static desktop last_sb holds the same image, so an
			 * occasional torn copy from a recycled source is invisible.
			 */
			if (last_sb != cur_sb)
				memcpy(cur_sb->vaddr, last_sb->vaddr, stream->buf_size);

			spin_lock_irq(&stream->list_lock);
			list_add_tail(&cur_sb->list, &stream->wait_list);
			spin_unlock_irq(&stream->list_lock);
		} else {
			/* No buffer available at all - should not happen */
			spin_unlock_irq(&stream->list_lock);
			continue;
		}

		data_urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!data_urb) {
			dev_err(&usb_dev->dev, "Data URB allocation error\n");
			WRITE_ONCE(stream->enabled, false);
			return;
		}

		/*
		 * Endpoint 1 bulk out. We store pointer to current stream buffer structure in
		 * transfer_buffer field of URB which is unused due to SGT.
		 */
		usb_fill_bulk_urb(data_urb, usb_dev, usb_sndbulkpipe(usb_dev, 1), cur_sb,
				  (int)stream->buf_size, fl2000_stream_data_completion, stream);
		data_urb->interval = 0;
		data_urb->sg = cur_sb->sgt.sgl;
		data_urb->num_sgs = cur_sb->sgt.nents;
		data_urb->transfer_flags |= URB_ZERO_PACKET;

		usb_anchor_urb(data_urb, &stream->anchor);
		ret = fl2000_submit_urb(data_urb);
		if (ret) {
			dev_err(&usb_dev->dev, "Data URB error %d\n", ret);
			usb_free_urb(data_urb);
			WRITE_ONCE(stream->enabled, false);
		}
	}
}

static void fl2000_xrgb8888_to_rgb888_line(u8 *dbuf, u32 *sbuf, u32 pixels)
{
	unsigned int xx = 0;

	for (unsigned int x = 0; x < pixels; x++) {
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x000000FF) >> 0;
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x0000FF00) >> 8;
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x00FF0000) >> 16;
	}
}

static void fl2000_xrgb8888_to_rgb565_line(u16 *dbuf, u32 *sbuf, u32 pixels)
{
	for (unsigned int x = 0; x < pixels; x++) {
		u16 val565 = ((sbuf[x] & 0x00F80000) >> 8) | ((sbuf[x] & 0x0000FC00) >> 5) |
			     ((sbuf[x] & 0x000000F8) >> 3);
		dbuf[x ^ 2] = val565;
	}
}

void fl2000_stream_compress(struct fl2000_stream *stream, void *src, unsigned int height,
			    unsigned int width, unsigned int pitch)
{
	struct fl2000_stream_buf *cur_sb;
	void *dst;
	u32 dst_line_len;

	/* Take a free buffer and own it (off all lists) for the conversion */
	spin_lock_irq(&stream->list_lock);
	if (list_empty(&stream->render_list)) {
		/* No free buffer: drop this frame rather than crash */
		spin_unlock_irq(&stream->list_lock);
		return;
	}
	cur_sb = list_first_entry(&stream->render_list, struct fl2000_stream_buf, list);
	list_del_init(&cur_sb->list);
	spin_unlock_irq(&stream->list_lock);

	/*
	 * A whole-frame pixel format conversion is several MB of work and MUST
	 * run with interrupts enabled - holding list_lock (which disables IRQs)
	 * across it starves the system (e.g. amdgpu IRQs -> fence fallback
	 * timers, laggy desktop). cur_sb is owned here so no lock is needed.
	 */
	dst = cur_sb->vaddr;
	dst_line_len = width * stream->bytes_pix;

	for (unsigned int y = 0; y < height; y++) {
		switch (stream->bytes_pix) {
		case 2:
			fl2000_xrgb8888_to_rgb565_line(dst, src, width);
			break;
		case 3:
			fl2000_xrgb8888_to_rgb888_line(dst, src, width);
			break;
		default:
			/* Rejected by fl2000_get_bytes_pix() */
			break;
		}
		src += pitch;
		dst += dst_line_len;
	}

	spin_lock_irq(&stream->list_lock);
	list_add_tail(&cur_sb->list, &stream->transmit_list);
	spin_unlock_irq(&stream->list_lock);
}

int fl2000_stream_mode_set(struct fl2000_stream *stream, int pixels, u32 bytes_pix)
{
	int ret;
	unsigned int size;

	/* Round buffer size up to multiple of 8 to meet HW expectations */
	size = ALIGN(pixels * bytes_pix, 8);

	stream->bytes_pix = bytes_pix;

	/* If there are buffers with same size - keep them */
	if (stream->buf_size == size)
		return 0;

	/* Destroy wrong size buffers if they exist */
	if (!list_empty(&stream->render_list))
		fl2000_stream_put_buffers(stream);

	/* Allocate new buffers */
	ret = fl2000_stream_get_buffers(stream, size);
	if (ret) {
		fl2000_stream_put_buffers(stream);
		stream->buf_size = 0;
		return ret;
	}

	stream->buf_size = size;

	return 0;
}

int fl2000_stream_enable(struct fl2000_stream *stream)
{
	if (WARN_ON(list_empty(&stream->render_list)))
		return -ENOMEM;

	/* Start the transmit work with the minimum number of buffers in flight */
	atomic_set(&stream->credits, FL2000_SB_MIN);
	WRITE_ONCE(stream->enabled, true);
	queue_work(stream->work_queue, &stream->work);

	return 0;
}

void fl2000_stream_disable(struct fl2000_stream *stream)
{
	struct fl2000_stream_buf *cur_sb;

	/* Wake the work up even when no URB is in flight to complete */
	WRITE_ONCE(stream->enabled, false);
	wake_up(&stream->credits_wq);

	drain_workqueue(stream->work_queue);

	if (!usb_wait_anchor_empty_timeout(&stream->anchor, 1000))
		usb_kill_anchored_urbs(&stream->anchor);

	spin_lock_irq(&stream->list_lock);
	while (!list_empty(&stream->transmit_list)) {
		cur_sb = list_first_entry(&stream->transmit_list, struct fl2000_stream_buf, list);
		list_move_tail(&cur_sb->list, &stream->render_list);
	}
	while (!list_empty(&stream->wait_list)) {
		cur_sb = list_first_entry(&stream->wait_list, struct fl2000_stream_buf, list);
		list_move_tail(&cur_sb->list, &stream->render_list);
	}
	spin_unlock_irq(&stream->list_lock);
}

/**
 * fl2000_stream_create() - streaming processing context creation
 * @usb_dev: FL2000 USB device
 * @crtc: CRTC that receives the vblank events of this stream
 *
 * Called on DRM master bind. It shall not initiate any USB transfers: buffers
 * and URBs are allocated later, once the mode (and thus the frame size) is
 * known.
 *
 * Return: stream context, or an ERR_PTR() on failure
 */
struct fl2000_stream *fl2000_stream_create(struct usb_device *usb_dev, struct drm_crtc *crtc)
{
	int ret;
	struct fl2000_stream *stream;

	/* Altsetting 1 on interface 0 */
	ret = usb_set_interface(usb_dev, FL2000_USBIF_AVCONTROL, 1);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot set streaming interface for bulk transfers\n");
		return ERR_PTR(ret);
	}

	stream = devres_alloc(&fl2000_stream_release, sizeof(*stream), GFP_KERNEL);
	if (!stream)
		return ERR_PTR(-ENOMEM);

	INIT_WORK(&stream->work, &fl2000_stream_work);
	INIT_LIST_HEAD(&stream->render_list);
	INIT_LIST_HEAD(&stream->transmit_list);
	INIT_LIST_HEAD(&stream->wait_list);
	spin_lock_init(&stream->list_lock);
	init_usb_anchor(&stream->anchor);
	atomic_set(&stream->credits, 0);
	init_waitqueue_head(&stream->credits_wq);
	stream->usb_dev = usb_dev;
	stream->crtc = crtc;

	stream->work_queue = alloc_ordered_workqueue("fl2000_stream", 0);
	if (!stream->work_queue) {
		devres_free(stream);
		return ERR_PTR(-ENOMEM);
	}

	devres_add(&usb_dev->dev, stream);

	return stream;
}

void fl2000_stream_destroy(struct usb_device *usb_dev)
{
	devres_release(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
}
