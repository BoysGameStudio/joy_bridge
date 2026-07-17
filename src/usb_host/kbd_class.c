/*
 * USB HID boot-protocol keyboard host class driver.
 *
 * Claims HID boot-keyboard interfaces (interface class 0x03, subclass 0x01
 * boot, protocol 0x01 keyboard) on the Zephyr USB host stack. On match it
 * puts the interface in boot protocol, opens its interrupt-IN endpoint,
 * polls it for 8-byte boot reports (modifier, reserved, 6 key slots), and
 * forwards each report to the input layer via kbd_map_report().
 *
 * Built only when CONFIG_JOY_BRIDGE_USB_KBD=y. Uses the experimental USB host
 * stack's private class API (subsys/usb/host) — the same headers the in-tree
 * UVC host class driver uses; there is no public class-driver API yet (and no
 * in-tree USB host HID class at all).
 */

#include <zephyr/kernel.h>

#ifdef CONFIG_JOY_BRIDGE_USB_KBD

#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/usb/usbh.h>
#include <zephyr/usb/class/hid.h>
#include <zephyr/drivers/usb/uhc.h>

#include "usbh_device.h" /* usbh_xfer_* helpers */
#include "usbh_class.h"  /* USBH_CLASS_MATCH_*, USBH_CLASS_IFNUM_DEVICE */
#include "usbh_ch9.h"    /* usbh_req_setup() */

#include "input/kbd_map.h"
#include <journal/journal.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(jb_usb, CONFIG_JOY_BRIDGE_LOG_LEVEL);

/* HID boot-keyboard interface signature. */
#define KBD_IF_CLASS 0x03 /* USB_BCC_HID */
#define KBD_IF_SUBCLASS 0x01 /* boot interface subclass */
#define KBD_IF_PROTOCOL HID_BOOT_IFACE_CODE_KEYBOARD /* 0x01 */

/* Minimum boot-protocol keyboard report: modifier, reserved, 6 key slots. */
#define KBD_BOOT_REPORT_LEN 8

/* Depth of the interrupt-IN transfer pool. Boot keyboards poll at bInterval
 * >= 8 ms and human input rates, so 2 transfers suffice to keep one queued
 * while one completes (same pipelining rationale as the pad's 4, scaled to
 * the rate). Only one device can be attached at a time (no hub), so UHC pool
 * pressure is not additive with the XInput driver. */
#define KBD_NUM_XFERS 2
/* Consecutive endpoint stalls tolerated before the poll is abandoned. */
#define KBD_STALL_MAX 3

struct kbd_instance {
	struct usb_device *udev;
	struct uhc_transfer *xfers[KBD_NUM_XFERS];
	uint16_t ep_mps;
	uint8_t stalls; /* consecutive -EPIPEs, for the recovery bound */
	bool running;
};

static struct kbd_instance kbd_inst;

/*
 * Walk the active configuration descriptor for the interrupt-IN endpoint of
 * the target boot-keyboard interface. iface == USBH_CLASS_IFNUM_DEVICE means
 * "first boot-keyboard interface". Returns true and fills *real_iface (the
 * bInterfaceNumber actually matched, for the class-request wIndex) plus
 * *ep_addr / *ep_mps / *ep_interval on success.
 */
static bool kbd_find_in_ep(struct usb_device *udev, uint8_t iface,
			   uint8_t *real_iface, uint8_t *ep_addr,
			   uint16_t *ep_mps, uint8_t *ep_interval)
{
	const uint8_t *p = udev->cfg_desc;

	if (p == NULL) {
		return false;
	}

	const struct usb_cfg_descriptor *cfg = (const void *)p;
	uint16_t total = sys_le16_to_cpu(cfg->wTotalLength);
	size_t off = 0;
	bool in_target = false;
	uint8_t target_ifnum = 0;

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
			bool is_boot_kbd = (id->bInterfaceClass == KBD_IF_CLASS &&
					    id->bInterfaceSubClass == KBD_IF_SUBCLASS &&
					    id->bInterfaceProtocol == KBD_IF_PROTOCOL);

			in_target = (iface == USBH_CLASS_IFNUM_DEVICE)
					    ? is_boot_kbd
					    : (id->bInterfaceNumber == iface);
			if (in_target) {
				target_ifnum = id->bInterfaceNumber;
			}
		} else if (in_target && h->bDescriptorType == USB_DESC_ENDPOINT &&
			   h->bLength >= sizeof(struct usb_ep_descriptor)) {
			const struct usb_ep_descriptor *ep = (const void *)(p + off);

			if ((ep->bEndpointAddress & USB_EP_DIR_IN) &&
			    (ep->bmAttributes & USB_EP_TRANSFER_TYPE_MASK) ==
				    USB_EP_TYPE_INTERRUPT) {
				*real_iface = target_ifnum;
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

/* Free every transfer in the pool (each buffer is released with its transfer). */
static void kbd_free_pool(struct usb_device *udev, struct kbd_instance *inst)
{
	for (int i = 0; i < KBD_NUM_XFERS; i++) {
		if (inst->xfers[i] != NULL) {
			usbh_xfer_free(udev, inst->xfers[i]);
			inst->xfers[i] = NULL;
		}
	}
}

/* Interrupt-IN completion: parse the report, then re-arm THIS transfer, which
 * replenishes its slot while the rest of the pool keeps the channel polling. */
static int kbd_in_cb(struct usb_device *udev, struct uhc_transfer *xfer)
{
	struct kbd_instance *inst = xfer->priv;

	if (inst == NULL || !inst->running) {
		return 0;
	}

	/* DIAG: surface the silent failure modes — errored completions and short
	 * reports are otherwise invisible (the poll just re-arms). Rate-limited to
	 * every 32nd occurrence per class so a continuous error loop is countable
	 * without flooding the log. */
	if (xfer->err != 0) {
		static uint32_t err_seen[8];
		uint8_t slot = (uint8_t)(-xfer->err < 8 ? -xfer->err : 8) - 1u;
		if ((++err_seen[slot] & 0x1Fu) == 1u) {
			LOG_WRN("kbd IN xfer err=%d x%u", xfer->err, err_seen[slot]);
		}
	} else if (xfer->buf != NULL && xfer->buf->len < KBD_BOOT_REPORT_LEN) {
		static uint32_t short_seen;
		if ((++short_seen & 0x1Fu) == 1u) {
			LOG_WRN("kbd IN short len=%u x%u", xfer->buf->len, short_seen);
		}
	}

	if (xfer->err == 0 && xfer->buf != NULL &&
	    xfer->buf->len >= KBD_BOOT_REPORT_LEN) {
		kbd_map_report(xfer->buf->data, xfer->buf->len);
	}

	/* Device going away: stop re-arming (terminal). A stalled endpoint
	 * (-EPIPE) is recoverable instead: CLEAR_FEATURE(ENDPOINT_HALT) resets
	 * both sides' data toggles, the driver reconfigures the channel on the
	 * next transfer, and the poll resumes. Give up after KBD_STALL_MAX
	 * consecutive stalls (a genuinely broken endpoint would loop forever).
	 * Transient errors fall through and the interrupt poll is re-armed. */
	if (xfer->err == -ECONNRESET || xfer->err == -ESHUTDOWN ||
	    xfer->err == -ENODEV) {
		return 0;
	}
	if (xfer->err == -EPIPE) {
		inst->stalls++;
		if (inst->stalls > KBD_STALL_MAX) {
			LOG_WRN("kbd IN endpoint stalled x%u; stopping poll", inst->stalls);
			inst->running = false;
			return 0;
		}
		const int rc = usbh_req_clear_sfs_halt(udev, xfer->ep);
		LOG_WRN("kbd IN stall #%u: clear_halt rc=%d; resuming poll",
			inst->stalls, rc);
	} else if (xfer->err == 0) {
		inst->stalls = 0;
	}

	if (xfer->buf != NULL) {
		net_buf_reset(xfer->buf);
	}
	(void)usbh_xfer_enqueue(udev, xfer);

	return 0;
}

/* Issue one class request to the interface. A STALL (-EPIPE) is legal here —
 * real boot keyboards may STALL SET_PROTOCOL/SET_IDLE — so it is logged and
 * swallowed (the UHC driver's silent control-retry already never retries
 * STALL). Any other negative error fails the probe. */
static int kbd_class_req(struct usb_device *udev, uint8_t request,
			 uint16_t value, uint16_t iface, const char *name)
{
	/* bmRequestType 0x21: host-to-device, class, interface. */
	int ret = usbh_req_setup(udev, 0x21, request, value, iface, 0, NULL);

	if (ret == -EPIPE) {
		LOG_WRN("%s stalled (iface %u); continuing", name, iface);
		return 0;
	}
	if (ret < 0) {
		LOG_ERR("%s failed on iface %u: %d", name, iface, ret);
		return ret;
	}
	return 0;
}

static int kbd_probe(struct usbh_class_data *const c_data,
		     struct usb_device *const udev, const uint8_t iface)
{
	struct kbd_instance *inst = c_data->priv;
	uint8_t real_iface = 0;
	uint8_t ep_addr = 0;
	uint16_t ep_mps = 0;
	uint8_t ep_interval = 0;

	if (udev == NULL || udev->state != USB_STATE_CONFIGURED) {
		return -ENODEV;
	}
	if (inst->udev != NULL) {
		/* Already bound to a keyboard (single instance). */
		return -ENOTSUP;
	}
	if (!kbd_find_in_ep(udev, iface, &real_iface, &ep_addr, &ep_mps,
			    &ep_interval)) {
		LOG_DBG("no kbd interrupt-IN endpoint on iface %u", iface);
		return -ENOTSUP;
	}

	/* Boot protocol, then report-at-your-own-rate. Both tolerate STALL. */
	int ret = kbd_class_req(udev, USB_HID_SET_PROTOCOL, HID_PROTOCOL_BOOT,
				real_iface, "SET_PROTOCOL(BOOT)");
	if (ret < 0) {
		return ret;
	}
	ret = kbd_class_req(udev, USB_HID_SET_IDLE, 0, real_iface,
			    "SET_IDLE(0)");
	if (ret < 0) {
		return ret;
	}

	inst->udev = udev;
	inst->ep_mps = ep_mps;
	inst->running = true;

	/* Allocate the transfer pool (each transfer gets its own buffer, sized to
	 * the endpoint MPS — usually 8 for a boot keyboard, but not guaranteed).
	 * A partial pool (if the UHC pools are exhausted) still works, just
	 * shallower. */
	for (int i = 0; i < KBD_NUM_XFERS; i++) {
		struct uhc_transfer *xfer =
			usbh_xfer_alloc(udev, ep_addr, kbd_in_cb, inst);
		if (xfer == NULL) {
			break;
		}
		xfer->type = USB_EP_TYPE_INTERRUPT;
		xfer->mps = ep_mps;
		xfer->interval = ep_interval;

		struct net_buf *buf = usbh_xfer_buf_alloc(udev, ep_mps);
		if (buf == NULL) {
			usbh_xfer_free(udev, xfer);
			break;
		}
		usbh_xfer_buf_add(udev, xfer, buf);
		inst->xfers[i] = xfer;
	}

	/* Submit them all so the controller polls a continuously-queued pipeline. */
	int submitted = 0;
	for (int i = 0; i < KBD_NUM_XFERS; i++) {
		if (inst->xfers[i] != NULL &&
		    usbh_xfer_enqueue(udev, inst->xfers[i]) == 0) {
			submitted++;
		}
	}
	if (submitted == 0) {
		LOG_ERR("failed to submit any kbd IN transfer");
		inst->running = false;
		kbd_free_pool(udev, inst);
		inst->udev = NULL;
		return -EIO;
	}

	JOURNAL(PAD, Display, All, "kbd bound iface=%u ep=0x%02x mps=%u bInt=%u pool=%d/%d",
		real_iface, ep_addr, ep_mps, ep_interval, submitted,
		KBD_NUM_XFERS);
	return 0;
}

static int kbd_removed(struct usbh_class_data *const c_data)
{
	struct kbd_instance *inst = c_data->priv;
	struct usb_device *udev = inst->udev;

	inst->running = false;
	for (int i = 0; i < KBD_NUM_XFERS; i++) {
		if (inst->xfers[i] != NULL && udev != NULL) {
			(void)usbh_xfer_dequeue(udev, inst->xfers[i]);
			usbh_xfer_free(udev, inst->xfers[i]);
		}
		inst->xfers[i] = NULL;
	}
	inst->udev = NULL;

	/* Park the emulated controller at neutral so a removed keyboard does not
	 * leave buttons stuck pressed on the console. */
	kbd_map_neutral();
	JOURNAL(PAD, Display, All, "kbd removed");
	return 0;
}

static int kbd_init(struct usbh_class_data *const c_data)
{
	ARG_UNUSED(c_data);
	return 0;
}

static struct usbh_class_api kbd_class_api = {
	.init = kbd_init,
	.probe = kbd_probe,
	.removed = kbd_removed,
};

static struct usbh_class_filter kbd_filters[] = {
	{
		.class = KBD_IF_CLASS,
		.sub = KBD_IF_SUBCLASS,
		.proto = KBD_IF_PROTOCOL,
		.flags = USBH_CLASS_MATCH_CODE_TRIPLE,
	},
	{0}, /* terminator (flags == 0) */
};

USBH_DEFINE_CLASS(kbd_hid_host, &kbd_class_api, &kbd_inst, kbd_filters);

#endif /* CONFIG_JOY_BRIDGE_USB_KBD */
