/*
 * XInput (Xbox 360 protocol) USB host class driver.
 *
 * XInput is vendor-specific (interface class 0xFF, subclass 0x5D, protocol
 * 0x01), not USB-HID, so it needs a dedicated class driver on the Zephyr USB
 * host stack. On match it opens the interface's interrupt-IN endpoint, polls it
 * for 20-byte gamepad reports, and forwards each report to the Pro2 HID engine
 * via xinput_map_report().
 *
 * Built only when CONFIG_JOY_BRIDGE_USB_XINPUT=y. Uses the experimental USB host
 * stack's private class API (subsys/usb/host) — the same headers the in-tree
 * UVC host class driver uses; there is no public class-driver API yet.
 */

#include <zephyr/kernel.h>

#ifdef CONFIG_JOY_BRIDGE_USB_XINPUT

#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/usb/usbh.h>
#include <zephyr/drivers/usb/uhc.h>

#include "usbh_device.h" /* usbh_xfer_* helpers */
#include "usbh_class.h"  /* USBH_CLASS_MATCH_*, USBH_CLASS_IFNUM_DEVICE */

#include "input/xinput.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(jb_xinput_class, CONFIG_JOY_BRIDGE_LOG_LEVEL);

/* XInput (Xbox 360) interface signature. */
#define XINPUT_IF_CLASS 0xFF
#define XINPUT_IF_SUBCLASS 0x5D
#define XINPUT_IF_PROTOCOL 0x01

struct xinput_instance {
	struct usb_device *udev;
	struct uhc_transfer *xfer;
	uint16_t ep_mps;
	bool running;
};

static struct xinput_instance xinput_inst;

/*
 * Walk the active configuration descriptor for the interrupt-IN endpoint of the
 * target interface. iface == USBH_CLASS_IFNUM_DEVICE means "first XInput
 * interface". Returns true and fills *ep_addr / *ep_mps / *ep_interval on
 * success.
 */
static bool xinput_find_in_ep(struct usb_device *udev, uint8_t iface,
			      uint8_t *ep_addr, uint16_t *ep_mps, uint8_t *ep_interval)
{
	const uint8_t *p = udev->cfg_desc;

	if (p == NULL) {
		return false;
	}

	const struct usb_cfg_descriptor *cfg = (const void *)p;
	uint16_t total = sys_le16_to_cpu(cfg->wTotalLength);
	size_t off = 0;
	bool in_target = false;

	while (off + sizeof(struct usb_desc_header) <= total) {
		const struct usb_desc_header *h = (const void *)(p + off);

		/* Stop on a too-short header or a descriptor whose claimed length runs
		 * past the configuration buffer. */
		if (h->bLength < sizeof(struct usb_desc_header) ||
		    off + h->bLength > total) {
			break;
		}

		if (h->bDescriptorType == USB_DESC_INTERFACE &&
		    h->bLength >= sizeof(struct usb_if_descriptor)) {
			const struct usb_if_descriptor *id = (const void *)(p + off);
			bool is_xinput = (id->bInterfaceClass == XINPUT_IF_CLASS &&
					  id->bInterfaceSubClass == XINPUT_IF_SUBCLASS &&
					  id->bInterfaceProtocol == XINPUT_IF_PROTOCOL);

			in_target = (iface == USBH_CLASS_IFNUM_DEVICE)
					    ? is_xinput
					    : (id->bInterfaceNumber == iface);
		} else if (in_target && h->bDescriptorType == USB_DESC_ENDPOINT &&
			   h->bLength >= sizeof(struct usb_ep_descriptor)) {
			const struct usb_ep_descriptor *ep = (const void *)(p + off);

			if ((ep->bEndpointAddress & USB_EP_DIR_IN) &&
			    (ep->bmAttributes & USB_EP_TRANSFER_TYPE_MASK) ==
				    USB_EP_TYPE_INTERRUPT) {
				*ep_addr = ep->bEndpointAddress;
				*ep_mps = sys_le16_to_cpu(ep->wMaxPacketSize) & 0x07FF;
				*ep_interval = ep->bInterval;
				return true;
			}
		}

		off += h->bLength;
	}

	return false;
}

/* Submit (or re-submit) the interrupt-IN polling transfer. */
static int xinput_submit(struct xinput_instance *inst)
{
	return usbh_xfer_enqueue(inst->udev, inst->xfer);
}

/* Interrupt-IN completion: parse the report, then re-arm the poll. */
static int xinput_in_cb(struct usb_device *udev, struct uhc_transfer *xfer)
{
	struct xinput_instance *inst = xfer->priv;

	if (inst == NULL || !inst->running) {
		return 0;
	}

	if (xfer->err == 0 && xfer->buf != NULL &&
	    xfer->buf->len >= sizeof(xinput_report_t)) {
		const xinput_report_t *r = (const void *)xfer->buf->data;

		/* Gamepad input report only: type 0x00, length 0x14 (ignore LED/rumble
		 * acks) — matches the verified upstream xinput_host.c check. */
		if (r->type == 0x00 && r->size == 0x14) {
			xinput_map_report(r);
		}
	}

	/* Stop re-arming on a terminal condition: device going away, or the endpoint
	 * stalled (-EPIPE) — re-submitting a halted endpoint without a CLEAR_FEATURE
	 * (not issued here) would just immediately re-stall. Transient errors fall
	 * through and the interrupt poll is re-armed. */
	if (xfer->err == -ECONNRESET || xfer->err == -ESHUTDOWN ||
	    xfer->err == -ENODEV || xfer->err == -EPIPE) {
		if (xfer->err == -EPIPE) {
			LOG_WRN("XInput IN endpoint stalled; stopping poll");
			inst->running = false;
		}
		return 0;
	}

	if (xfer->buf != NULL) {
		net_buf_reset(xfer->buf);
	}
	(void)xinput_submit(inst);

	return 0;
}

static int xinput_probe(struct usbh_class_data *const c_data,
			struct usb_device *const udev, const uint8_t iface)
{
	struct xinput_instance *inst = c_data->priv;
	uint8_t ep_addr = 0;
	uint16_t ep_mps = 0;
	uint8_t ep_interval = 0;

	if (udev == NULL || udev->state != USB_STATE_CONFIGURED) {
		return -ENODEV;
	}
	if (inst->udev != NULL) {
		/* Already bound to a controller (single instance). */
		return -ENOTSUP;
	}
	if (!xinput_find_in_ep(udev, iface, &ep_addr, &ep_mps, &ep_interval)) {
		LOG_DBG("no XInput interrupt-IN endpoint on iface %u", iface);
		return -ENOTSUP;
	}

	struct uhc_transfer *xfer =
		usbh_xfer_alloc(udev, ep_addr, xinput_in_cb, inst);
	if (xfer == NULL) {
		return -ENOMEM;
	}
	xfer->type = USB_EP_TYPE_INTERRUPT;
	xfer->mps = ep_mps;
	xfer->interval = ep_interval;

	struct net_buf *buf = usbh_xfer_buf_alloc(udev, ep_mps);
	if (buf == NULL) {
		usbh_xfer_free(udev, xfer);
		return -ENOMEM;
	}
	usbh_xfer_buf_add(udev, xfer, buf);

	inst->udev = udev;
	inst->xfer = xfer;
	inst->ep_mps = ep_mps;
	inst->running = true;

	int ret = xinput_submit(inst);
	if (ret != 0) {
		LOG_ERR("failed to submit XInput IN transfer: %d", ret);
		inst->running = false;
		inst->udev = NULL;
		inst->xfer = NULL;
		usbh_xfer_free(udev, xfer);
		return ret;
	}

	LOG_INF("XInput controller bound (iface %u, ep 0x%02x, mps %u)",
		iface, ep_addr, ep_mps);
	return 0;
}

static int xinput_removed(struct usbh_class_data *const c_data)
{
	struct xinput_instance *inst = c_data->priv;

	inst->running = false;
	if (inst->xfer != NULL && inst->udev != NULL) {
		(void)usbh_xfer_dequeue(inst->udev, inst->xfer);
		usbh_xfer_free(inst->udev, inst->xfer);
	}
	inst->xfer = NULL;
	inst->udev = NULL;

	/* Park the emulated controller at neutral so a removed pad does not leave
	 * buttons stuck pressed on the console. */
	xinput_map_neutral();
	LOG_INF("XInput controller removed");
	return 0;
}

static int xinput_init(struct usbh_class_data *const c_data)
{
	ARG_UNUSED(c_data);
	return 0;
}

static struct usbh_class_api xinput_class_api = {
	.init = xinput_init,
	.probe = xinput_probe,
	.removed = xinput_removed,
};

static struct usbh_class_filter xinput_filters[] = {
	{
		.class = XINPUT_IF_CLASS,
		.sub = XINPUT_IF_SUBCLASS,
		.proto = XINPUT_IF_PROTOCOL,
		.flags = USBH_CLASS_MATCH_CODE_TRIPLE,
	},
	{0}, /* terminator (flags == 0) */
};

USBH_DEFINE_CLASS(xinput_host, &xinput_class_api, &xinput_inst, xinput_filters);

#endif /* CONFIG_JOY_BRIDGE_USB_XINPUT */
