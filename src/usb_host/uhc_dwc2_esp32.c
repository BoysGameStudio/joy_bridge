/*
 * Native ESP32-S3 USB-OTG host controller (UHC) driver.
 *
 * Implements the Zephyr USB host controller API (struct uhc_api) over the
 * vendored Espressif host USB HAL (usb_dwc_hal) + USB PHY helper (usb_phy).
 * Modelled on drivers/usb/uhc/uhc_max3421e.c: a worker thread, woken by the
 * peripheral ISR, decodes HAL port/channel events and forwards them to the USB
 * host stack via uhc_submit_event() / uhc_xfer_return().
 *
 * Transfer engine: single-active (one channel processes one uhc_transfer at a
 * time) — the model the UHC layer is built around. Control transfers are run
 * as SETUP/DATA/STATUS channel activations; bulk/interrupt as a single data
 * stage, using scatter/gather DMA QTDs.
 *
 * Status: verified working end-to-end on hardware (ESP32-S3-DevKitC-1 + a wired
 * XInput pad; see AGENTS.md). Periodic (frame-list) scheduling for interrupt
 * endpoints is implemented below. Full-Speed only. Remaining bring-up note: IN
 * transfers pass net_buf tailroom verbatim (not rounded up to MPS) — fine for
 * the control/interrupt endpoints used here.
 */

#define DT_DRV_COMPAT espressif_esp32_usb_otg

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <string.h>

#include <zephyr/net_buf.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/usb/usb_ch9.h>

#include "uhc_common.h"
#include "uhc_dwc2_esp32.h"

#include <hal/usb_dwc_hal.h>
#include <hal/usb_dwc_types.h>
#include <hal/usb_dwc_ll.h>
#include <esp_private/usb_phy.h>
#include <esp_intr_alloc.h>
#include <esp_err.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(uhc_dwc2_esp32, CONFIG_UHC_DRIVER_LOG_LEVEL);

#ifdef CONFIG_JOY_BRIDGE_DIAG_LATENCY
#include <zephyr/sys/printk.h>
/* Latency localisation: time from an interrupt-IN completion to the next re-arm
 * (the single-active SW round-trip). Logged once/sec via printk so it does not
 * depend on the UHC log level. */
static uint32_t diag_cplt_cyc;
static uint64_t diag_rearm_sum_us;
static uint32_t diag_rearm_n;
static int64_t  diag_win_ms;

static inline void diag_mark_cplt(const struct uhc_transfer *xfer)
{
	if (xfer->type == USB_EP_TYPE_INTERRUPT) {
		diag_cplt_cyc = k_cycle_get_32();
	}
}

static inline void diag_mark_rearm(const struct uhc_transfer *xfer)
{
	if (xfer->type != USB_EP_TYPE_INTERRUPT || diag_cplt_cyc == 0) {
		return;
	}
	diag_rearm_sum_us += k_cyc_to_us_floor32(k_cycle_get_32() - diag_cplt_cyc);
	diag_rearm_n++;
	int64_t now = k_uptime_get();
	if (diag_win_ms == 0) {
		diag_win_ms = now;
	}
	if (now - diag_win_ms >= 1000) {
		printk("[USB] cplt->rearm avg=%u us  n=%u\n",
		       (uint32_t)(diag_rearm_sum_us / diag_rearm_n), diag_rearm_n);
		diag_rearm_sum_us = 0;
		diag_rearm_n = 0;
		diag_win_ms = now;
	}
}
#endif

static K_KERNEL_STACK_DEFINE(uhc_dwc2_thread_stack, 4096);

/*
 * Scatter/gather transfer-descriptor (QTD) list. The DWC core fetches this via
 * DMA, so it must live in DMA-capable internal RAM and be 512-byte aligned. One
 * QTD covers a whole stage/transfer (the core auto-splits into MPS packets).
 * Single-active engine -> one shared list is sufficient.
 */
#define UHC_DWC2_QTD_LIST_LEN 32 /* sized for periodic INTR (= frame-list length) */
#define UHC_DWC2_CTRL_LIST_LEN 3 /* one QTD per control stage (non-periodic) */
#define UHC_DWC2_FRAME_LIST_LEN USB_HAL_FRAME_LIST_LEN_32
static usb_dwc_ll_dma_qtd_t qtd_list[UHC_DWC2_QTD_LIST_LEN]
	__aligned(USB_DWC_QTD_LIST_MEM_ALIGN);

/* Periodic scheduling frame list (interrupt endpoints). 512-byte aligned,
 * DMA-capable internal RAM (.bss); no cache sync needed on the S3. */
static uint32_t frame_list[UHC_DWC2_FRAME_LIST_LEN]
	__aligned(USB_DWC_FRAME_LIST_MEM_ALIGN);

/* -------------------------------------------------------------------------- */
/* FIFO sizing                                                                */
/* -------------------------------------------------------------------------- */

/*
 * Program a balanced RX / non-periodic-TX / periodic-TX split from the core's
 * total FIFO depth. Validated against the HAL before applying; if invalid the
 * port-enable defaults are left in place.
 */
static void uhc_dwc2_set_fifo(struct uhc_dwc2_data *data)
{
	uint32_t total = data->hal.constant_config.fifo_size;

	if (total == 0) {
		return;
	}

	usb_dwc_hal_fifo_config_t cfg = {
		.rx_fifo_lines   = total / 2U,
		.nptx_fifo_lines = total / 4U,
		.ptx_fifo_lines  = total - (total / 2U) - (total / 4U),
	};

	if (usb_dwc_hal_fifo_config_is_valid(&data->hal, &cfg)) {
		usb_dwc_hal_set_fifo_config(&data->hal, &cfg);
	}
}

/* -------------------------------------------------------------------------- */
/* Transfer engine (single active transfer, one channel)                      */
/* -------------------------------------------------------------------------- */

/* Activate the channel for the active transfer's current stage. */
static void uhc_dwc2_begin_stage(struct uhc_dwc2_data *data)
{
	struct uhc_transfer *xfer = data->active;
	const bool is_ctrl = (USB_EP_GET_IDX(xfer->ep) == 0);
	uint32_t flags = USB_DWC_HAL_XFER_DESC_FLAG_HOC;
	uint8_t *buf_ptr = NULL;
	int len = 0;
	bool dir_in;
	/* desc_list_len = number of FILLED QTDs, matching the ESP-IDF HCD (which
	 * passes num_qtds for INTR). Bulk/interrupt here fill exactly one MPS-sized
	 * QTD. Passing the allocated array length (32) instead made the periodic
	 * scheduler cycle the channel through 32 mostly-null slots, servicing our QTD
	 * only ~once per frame-list wrap — capping the rate at ~10/s on a 1 ms
	 * (bInterval=1) endpoint. */
	const int list_len = is_ctrl ? UHC_DWC2_CTRL_LIST_LEN : 1;

	if (is_ctrl) {
		switch (xfer->stage) {
		case UHC_CONTROL_STAGE_SETUP:
			usb_dwc_hal_chan_set_dir(&data->chan, false);
			usb_dwc_hal_chan_set_pid(&data->chan, 0); /* SETUP is DATA0 */
			usb_dwc_hal_xfer_desc_fill(qtd_list, 0, xfer->setup_pkt,
						   sizeof(xfer->setup_pkt),
						   USB_DWC_HAL_XFER_DESC_FLAG_SETUP | flags);
			usb_dwc_hal_chan_activate(&data->chan, qtd_list, list_len, 0);
			return;

		case UHC_CONTROL_STAGE_DATA:
			dir_in = USB_EP_DIR_IS_IN(xfer->ep);
			usb_dwc_hal_chan_set_dir(&data->chan, dir_in);
			usb_dwc_hal_chan_set_pid(&data->chan, 1); /* data stage starts DATA1 */
			if (dir_in) {
				buf_ptr = net_buf_tail(xfer->buf);
				len = net_buf_tailroom(xfer->buf);
				flags |= USB_DWC_HAL_XFER_DESC_FLAG_IN;
			} else {
				buf_ptr = xfer->buf->data;
				len = xfer->buf->len;
			}
			usb_dwc_hal_xfer_desc_fill(qtd_list, 0, buf_ptr, len, flags);
			usb_dwc_hal_chan_activate(&data->chan, qtd_list, list_len, 0);
			return;

		case UHC_CONTROL_STAGE_STATUS:
		default:
			/* Status is opposite the data stage; IN when there was no data. */
			dir_in = (xfer->buf == NULL) ? true : !USB_EP_DIR_IS_IN(xfer->ep);
			usb_dwc_hal_chan_set_dir(&data->chan, dir_in);
			usb_dwc_hal_chan_set_pid(&data->chan, 1); /* status is DATA1 */
			if (dir_in) {
				flags |= USB_DWC_HAL_XFER_DESC_FLAG_IN;
			}
			usb_dwc_hal_xfer_desc_fill(qtd_list, 0, NULL, 0, flags);
			usb_dwc_hal_chan_activate(&data->chan, qtd_list, list_len, 0);
			return;
		}
	}

	/* Bulk / interrupt: a single data stage. */
#ifdef CONFIG_JOY_BRIDGE_DIAG_LATENCY
	diag_mark_rearm(xfer);
#endif
	dir_in = USB_EP_DIR_IS_IN(xfer->ep);
	usb_dwc_hal_chan_set_dir(&data->chan, dir_in);
	usb_dwc_hal_chan_set_pid(&data->chan, data->in_pid);
	if (dir_in) {
		buf_ptr = net_buf_tail(xfer->buf);
		len = net_buf_tailroom(xfer->buf);
		/* TODO(bring-up): round len up to a multiple of MPS for IN. */
		flags |= USB_DWC_HAL_XFER_DESC_FLAG_IN;
	} else {
		buf_ptr = xfer->buf->data;
		len = xfer->buf->len;
	}
	usb_dwc_hal_xfer_desc_fill(qtd_list, 0, buf_ptr, len, flags);
	usb_dwc_hal_chan_activate(&data->chan, qtd_list, list_len, 0);
}

/* Claim the next queued transfer if the channel is idle, and start it. */
static void uhc_dwc2_schedule(const struct device *dev)
{
	struct uhc_dwc2_data *data = uhc_get_private(dev);
	struct uhc_transfer *xfer;

	if (!data->chan_allocated || data->active != NULL) {
		return;
	}

	xfer = uhc_xfer_get_next(dev);
	if (xfer == NULL) {
		return;
	}

	data->active = xfer;

	/* Reconfigure the channel only when the target endpoint changes. Re-running
	 * set_ep_char / resetting the toggle on every interrupt re-poll would drop
	 * it from the periodic schedule and corrupt DATA0/DATA1. */
	if (xfer->ep != data->cur_ep || xfer->udev->addr != data->cur_addr ||
	    xfer->mps != data->cur_mps) {
		usb_dwc_hal_ep_char_t ep_char = {
			.type = (usb_dwc_xfer_type_t)xfer->type,
			.bEndpointAddress = xfer->ep,
			.mps = xfer->mps,
			.dev_addr = xfer->udev->addr,
			.ls_via_fs_hub = 0,
		};
		if (xfer->type == USB_EP_TYPE_INTERRUPT) {
			/* FS: bInterval is the polling interval in frames directly. */
			ep_char.periodic.interval = (xfer->interval > 0) ? xfer->interval : 1;
			ep_char.periodic.offset = 0;
			ep_char.periodic.is_hs = false;
		}
		usb_dwc_hal_chan_set_ep_char(&data->hal, &data->chan, &ep_char);
		data->cur_ep = xfer->ep;
		data->cur_addr = xfer->udev->addr;
		data->cur_mps = xfer->mps;
		data->in_pid = 0; /* fresh target -> DATA0 */
	}

	if (USB_EP_GET_IDX(xfer->ep) == 0) {
		xfer->stage = UHC_CONTROL_STAGE_SETUP;
	}

	uhc_dwc2_begin_stage(data);
}

/* Channel transfer-complete: parse the QTD, advance control stages or finish. */
static void uhc_dwc2_on_cplt(const struct device *dev)
{
	struct uhc_dwc2_data *data = uhc_get_private(dev);
	struct uhc_transfer *xfer = data->active;
	int rem_len = 0;
	int status = 0;

	if (xfer == NULL) {
		return;
	}

	usb_dwc_hal_xfer_desc_parse(qtd_list, 0, &rem_len, &status);

	if (status != USB_DWC_HAL_XFER_DESC_STS_SUCCESS) {
		uhc_xfer_return(dev, xfer, -EIO);
		data->active = NULL;
		return;
	}

	if (USB_EP_GET_IDX(xfer->ep) == 0) {
		switch (xfer->stage) {
		case UHC_CONTROL_STAGE_SETUP:
			xfer->stage = (xfer->buf != NULL) ? UHC_CONTROL_STAGE_DATA
							  : UHC_CONTROL_STAGE_STATUS;
			uhc_dwc2_begin_stage(data);
			return;
		case UHC_CONTROL_STAGE_DATA:
			if (USB_EP_DIR_IS_IN(xfer->ep)) {
				int rcvd = net_buf_tailroom(xfer->buf) - rem_len;

				if (rcvd > 0) {
					net_buf_add(xfer->buf, rcvd);
				}
			}
			xfer->stage = UHC_CONTROL_STAGE_STATUS;
			uhc_dwc2_begin_stage(data);
			return;
		case UHC_CONTROL_STAGE_STATUS:
		default:
			uhc_xfer_return(dev, xfer, 0);
			data->active = NULL;
			return;
		}
	}

	/* Bulk / interrupt: save the toggle, record received bytes, complete. */
#ifdef CONFIG_JOY_BRIDGE_DIAG_LATENCY
	diag_mark_cplt(xfer);
#endif
	data->in_pid = usb_dwc_hal_chan_get_pid(&data->chan);
	if (USB_EP_DIR_IS_IN(xfer->ep)) {
		int rcvd = net_buf_tailroom(xfer->buf) - rem_len;

		if (rcvd > 0) {
			net_buf_add(xfer->buf, rcvd);
		}
	}
	uhc_xfer_return(dev, xfer, 0);
	data->active = NULL;
}

/* -------------------------------------------------------------------------- */
/* Event handling (worker thread context)                                     */
/* -------------------------------------------------------------------------- */

static void uhc_dwc2_handle_chan_events(const struct device *dev)
{
	struct uhc_dwc2_data *data = uhc_get_private(dev);
	usb_dwc_hal_chan_t *chan;

	while ((chan = usb_dwc_hal_get_chan_pending_intr(&data->hal)) != NULL) {
		usb_dwc_hal_chan_event_t ev = usb_dwc_hal_chan_decode_intr(chan);

		switch (ev) {
		case USB_DWC_HAL_CHAN_EVENT_CPLT:
			uhc_dwc2_on_cplt(dev);
			break;
		case USB_DWC_HAL_CHAN_EVENT_ERROR: {
			usb_dwc_hal_chan_error_t e = usb_dwc_hal_chan_get_error(chan);
			int err = (e == USB_DWC_HAL_CHAN_ERROR_STALL) ? -EPIPE : -EIO;

			LOG_DBG("channel error e=%d -> %d", e, err);

			if (data->active != NULL) {
				uhc_xfer_return(dev, data->active, err);
				data->active = NULL;
			}
			break;
		}
		case USB_DWC_HAL_CHAN_EVENT_HALT_REQ:
			/* Completes a halt requested by uhc_dwc2_ep_dequeue() to cancel
			 * the active transfer; the channel is quiescent now, so it is
			 * safe to return it. */
			if (data->active != NULL && data->active->err == -ECONNRESET) {
				uhc_xfer_return(dev, data->active, -ECONNRESET);
				data->active = NULL;
			}
			break;
		case USB_DWC_HAL_CHAN_EVENT_NONE:
		default:
			break;
		}
	}
}

/*
 * Configure the port for operation and allocate the transfer channel. Idempotent
 * (guarded by chan_allocated) — reached from both bus_reset (synchronous poll)
 * and the async PORT_EVENT_ENABLED.
 */
static void uhc_dwc2_port_enabled(struct uhc_dwc2_data *data)
{
	if (data->chan_allocated) {
		return;
	}
	usb_dwc_hal_port_enable(&data->hal);
	uhc_dwc2_set_fifo(data);
	data->chan_allocated = usb_dwc_hal_chan_alloc(&data->hal, &data->chan,
						      (void *)data->dev);

	/* Enable periodic scheduling so interrupt endpoints get serviced. */
	memset(frame_list, 0, sizeof(frame_list));
	usb_dwc_hal_port_set_frame_list(&data->hal, frame_list, UHC_DWC2_FRAME_LIST_LEN);
	usb_dwc_hal_port_periodic_enable(&data->hal);

	data->cur_ep = 0xFF; /* invalidate -> first transfer configures the channel */
	data->in_pid = 0;

	LOG_DBG("port enabled: chan_allocated=%d fifo=%u", data->chan_allocated,
		data->hal.constant_config.fifo_size);
}

static void uhc_dwc2_handle_port_event(const struct device *dev)
{
	struct uhc_dwc2_data *data = uhc_get_private(dev);
	usb_dwc_hal_port_event_t ev = usb_dwc_hal_decode_intr(&data->hal);

	switch (ev) {
	case USB_DWC_HAL_PORT_EVENT_CHAN:
		uhc_dwc2_handle_chan_events(dev);
		break;

	case USB_DWC_HAL_PORT_EVENT_CONN:
		/* Debounce, then latch the connected device's speed and report it. */
		k_msleep(UHC_DWC2_DEBOUNCE_MS);
		if (usb_dwc_hal_port_check_if_connected(&data->hal)) {
			usb_dwc_speed_t sp = usb_dwc_hal_port_get_conn_speed(&data->hal);

			usb_dwc_hal_disable_debounce_lock(&data->hal);
			uhc_submit_event(dev,
					 (sp == USB_DWC_SPEED_LOW)
						 ? UHC_EVT_DEV_CONNECTED_LS
						 : UHC_EVT_DEV_CONNECTED_FS,
					 0);
		} else {
			usb_dwc_hal_disable_debounce_lock(&data->hal);
		}
		break;

	case USB_DWC_HAL_PORT_EVENT_DISCONN:
		usb_dwc_hal_disable_debounce_lock(&data->hal);
		/* An active control transfer is owned by the ch9 layer (freed via
		 * ch9_req_cb), not by the class removed() path, so completing it here
		 * is race-free and unblocks usbh_req_setup() immediately. Dropping it
		 * silently instead would strand the enumeration bus thread on
		 * ch9_req_sync for the full SETUP_REQ_TIMEOUT (5 s), delaying both this
		 * removal and any reconnect queued behind it. */
		if (data->active != NULL && USB_EP_GET_IDX(data->active->ep) == 0) {
			uhc_xfer_return(dev, data->active, -ENODEV);
		}
		/* Do NOT uhc_xfer_return() a class transfer here: it posts an
		 * EP_REQUEST carrying the transfer pointer that the class removed()
		 * callback then frees on the bus thread -> use-after-free when
		 * usbh_thread dispatches the stale event. Drop it as active and free
		 * the channel; class transfers stay linked for removed() to dequeue
		 * (now unlinking) and free. */
		data->active = NULL;
		if (data->chan_allocated) {
			/* Port error leaves the channel active with no halt IRQ; mark
			 * it halted so the HAL permits the free. */
			usb_dwc_hal_chan_mark_halted(&data->chan);
			usb_dwc_hal_chan_free(&data->hal, &data->chan);
			data->chan_allocated = false;
		}
		data->cur_ep = 0xFF;
		uhc_submit_event(dev, UHC_EVT_DEV_REMOVED, 0);
		break;

	case USB_DWC_HAL_PORT_EVENT_ENABLED:
		/* Backup path: bus_reset normally enables synchronously. Idempotent. */
		uhc_dwc2_port_enabled(data);
		uhc_submit_event(dev, UHC_EVT_RESETED, 0);
		break;

	case USB_DWC_HAL_PORT_EVENT_OVRCUR:
		uhc_submit_event(dev, UHC_EVT_ERROR, -EIO);
		break;

	case USB_DWC_HAL_PORT_EVENT_DISABLED:
	case USB_DWC_HAL_PORT_EVENT_OVRCUR_CLR:
	case USB_DWC_HAL_PORT_EVENT_REMOTE_WAKEUP:
	case USB_DWC_HAL_PORT_EVENT_NONE:
	default:
		break;
	}
}

static void uhc_dwc2_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	struct uhc_dwc2_data *data = uhc_get_private(dev);

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_sem_take(&data->evt_sem, K_FOREVER);

		uhc_lock_internal(dev, K_FOREVER);
		uhc_dwc2_handle_port_event(dev);
		uhc_dwc2_schedule(dev); /* claim a queued transfer if idle */
		uhc_unlock_internal(dev);

		/* Re-arm the source masked in the ISR. */
		esp_intr_enable(data->intr_handle);
	}
}

static void uhc_dwc2_isr(void *arg)
{
	const struct device *dev = arg;
	struct uhc_dwc2_data *data = uhc_get_private(dev);

	/* The DWC global interrupt is level-triggered; mask it and let the worker
	 * decode/clear the sources, then re-arm. Keeps the ISR tiny. */
	esp_intr_disable(data->intr_handle);
	k_sem_give(&data->evt_sem);
}

/* -------------------------------------------------------------------------- */
/* UHC API                                                                    */
/* -------------------------------------------------------------------------- */

static int uhc_dwc2_lock(const struct device *dev)
{
	return uhc_lock_internal(dev, K_FOREVER);
}

static int uhc_dwc2_unlock(const struct device *dev)
{
	return uhc_unlock_internal(dev);
}

static int uhc_dwc2_init(const struct device *dev)
{
	struct uhc_dwc2_data *data = uhc_get_private(dev);

	/* Bring up the HAL context (checks the core is alive, caches the HW config,
	 * switches to host mode). usb_dwc_hal_init needs >=25 ms before host mode
	 * takes effect (HAL contract / ESP-IDF INIT_DELAY_MS); without it the port
	 * will not enable after a bus reset. Then enable port/channel interrupts. */
	usb_dwc_hal_init(&data->hal, 0);
	data->hal.channels.hdls = data->chan_hdls;
	k_msleep(UHC_DWC2_INIT_DELAY_MS);
	usb_dwc_hal_port_init(&data->hal);

	return 0;
}

static int uhc_dwc2_enable(const struct device *dev)
{
	struct uhc_dwc2_data *data = uhc_get_private(dev);

	usb_dwc_hal_port_toggle_power(&data->hal, true);
	esp_intr_enable(data->intr_handle);

	return 0;
}

static int uhc_dwc2_disable(const struct device *dev)
{
	struct uhc_dwc2_data *data = uhc_get_private(dev);

	esp_intr_disable(data->intr_handle);
	usb_dwc_hal_port_toggle_power(&data->hal, false);

	return 0;
}

static int uhc_dwc2_shutdown(const struct device *dev)
{
	struct uhc_dwc2_data *data = uhc_get_private(dev);

	esp_intr_disable(data->intr_handle);
	usb_dwc_hal_port_deinit(&data->hal);
	usb_dwc_hal_deinit(&data->hal);

	return 0;
}

static int uhc_dwc2_bus_reset(const struct device *dev)
{
	struct uhc_dwc2_data *data = uhc_get_private(dev);

	/* Caller (uhc_do_reset) holds the driver lock. Drive SE0, hold >=10 ms,
	 * release; the port enables automatically afterwards. Poll the enable bit and
	 * configure the channel here, synchronously: while we hold the lock the worker
	 * can't run, and the one-shot enable-change interrupt is easily missed. */
	LOG_DBG("bus reset");
	usb_dwc_hal_port_toggle_reset(&data->hal, true);
	k_msleep(UHC_DWC2_RESET_HOLD_MS);
	usb_dwc_hal_port_toggle_reset(&data->hal, false);

	/* Pump decode_intr ourselves (the worker can't run while we hold the lock)
	 * here) to catch the post-reset port-enable the way the ISR would, and
	 * configure the channel synchronously. Up to ~100 ms. */
	bool enabled = false;

	for (int i = 0; i < 100; i++) {
		if (usb_dwc_hal_decode_intr(&data->hal) == USB_DWC_HAL_PORT_EVENT_ENABLED) {
			enabled = true;
			break;
		}
		k_msleep(1);
	}

	if (enabled) {
		k_msleep(20); /* reset-recovery before the device accepts transfers */
		uhc_dwc2_port_enabled(data);
	} else {
		LOG_ERR("port did not enable after bus reset");
	}

	uhc_submit_event(dev, UHC_EVT_RESETED, 0);
	return 0;
}

static int uhc_dwc2_sof_enable(const struct device *dev)
{
	/* SOF generation starts automatically once the port is enabled. */
	ARG_UNUSED(dev);
	return 0;
}

static int uhc_dwc2_bus_suspend(const struct device *dev)
{
	struct uhc_dwc2_data *data = uhc_get_private(dev);

	usb_dwc_hal_port_suspend(&data->hal);
	uhc_submit_event(dev, UHC_EVT_SUSPENDED, 0);

	return 0;
}

static int uhc_dwc2_bus_resume(const struct device *dev)
{
	struct uhc_dwc2_data *data = uhc_get_private(dev);

	usb_dwc_hal_port_toggle_resume(&data->hal, true);
	k_msleep(20);
	usb_dwc_hal_port_toggle_resume(&data->hal, false);
	uhc_submit_event(dev, UHC_EVT_RESUMED, 0);

	return 0;
}

static int uhc_dwc2_ep_enqueue(const struct device *dev,
			       struct uhc_transfer *const xfer)
{
	struct uhc_dwc2_data *data = uhc_get_private(dev);
	int ret = uhc_xfer_append(dev, xfer);

	if (ret == 0) {
		/* Wake the worker so an idle channel claims the new transfer. */
		k_sem_give(&data->evt_sem);
	}

	return ret;
}

static int uhc_dwc2_ep_dequeue(const struct device *dev,
			       struct uhc_transfer *const xfer)
{
	struct uhc_dwc2_data *data = uhc_get_private(dev);

	/* Runs under the UHC API mutex (uhc_ep_dequeue), mutually exclusive with
	 * the worker, so list / data->active access needs no further locking. The
	 * previous version only set ->err and never unlinked, so uhc_xfer_free()
	 * (which checks only ->queued) freed a still-linked node -> dangling list
	 * entry / heap corruption on the next attach. */

	if (data->active == xfer) {
		/* On the channel now: stop it before the caller frees the buffer.
		 * request_halt() is synchronous only if already halted; otherwise
		 * the halt completes via HALT_REQ, which returns the marked
		 * transfer (see uhc_dwc2_handle_chan_events). */
		xfer->err = -ECONNRESET;
		if (data->chan_allocated &&
		    !usb_dwc_hal_chan_request_halt(&data->chan)) {
			return 0;
		}
		data->active = NULL;
		if (sys_dnode_is_linked(&xfer->node)) {
			uhc_xfer_return(dev, xfer, -ECONNRESET);
		}
		return 0;
	}

	if (!sys_dnode_is_linked(&xfer->node)) {
		return 0;
	}

	if (USB_EP_GET_IDX(xfer->ep) == 0) {
		/* Control transfer: owned by the ch9 layer, which frees it from its
		 * completion callback. Unlink and fire the cancelled return. */
		uhc_xfer_return(dev, xfer, -ECONNRESET);
	} else {
		/* Class-endpoint transfer: the class owns the transfer/buffer and
		 * frees it directly after dequeue, so just unlink in place; firing
		 * a completion here would race that free. */
		sys_dlist_remove(&xfer->node);
	}

	return 0;
}

static const struct uhc_api uhc_dwc2_api = {
	.lock = uhc_dwc2_lock,
	.unlock = uhc_dwc2_unlock,
	.init = uhc_dwc2_init,
	.enable = uhc_dwc2_enable,
	.disable = uhc_dwc2_disable,
	.shutdown = uhc_dwc2_shutdown,
	.bus_reset = uhc_dwc2_bus_reset,
	.sof_enable = uhc_dwc2_sof_enable,
	.bus_suspend = uhc_dwc2_bus_suspend,
	.bus_resume = uhc_dwc2_bus_resume,
	.ep_enqueue = uhc_dwc2_ep_enqueue,
	.ep_dequeue = uhc_dwc2_ep_dequeue,
};

/* -------------------------------------------------------------------------- */
/* Device init (boot)                                                         */
/* -------------------------------------------------------------------------- */

static int uhc_dwc2_driver_init(const struct device *dev)
{
	const struct uhc_dwc2_config *cfg = dev->config;
	struct uhc_data *uhc = dev->data;
	struct uhc_dwc2_data *data = uhc->priv;
	int ret;

	data->dev = dev;
	k_mutex_init(&uhc->mutex);
	k_sem_init(&data->evt_sem, 0, 1);

	if (!device_is_ready(cfg->clock_dev)) {
		LOG_ERR("clock controller not ready");
		return -ENODEV;
	}

	ret = clock_control_on(cfg->clock_dev, cfg->clock_subsys);
	if (ret != 0 && ret != -EALREADY) {
		LOG_ERR("failed to enable USB peripheral clock: %d", ret);
		return ret;
	}

	/* Bring up the internal FS PHY in OTG host mode. This also enables the
	 * OTG controller (sets IDDIG host, drives VBUS-valid signalling). */
	usb_phy_config_t phy_cfg = {
		.controller = USB_PHY_CTRL_OTG,
		.target = USB_PHY_TARGET_INT,
		.otg_mode = USB_OTG_MODE_HOST,
		.otg_speed = USB_PHY_SPEED_FULL,
	};

	if (usb_new_phy(&phy_cfg, &data->phy) != ESP_OK) {
		LOG_ERR("usb_new_phy(host) failed");
		return -EIO;
	}

	/* Allocate the interrupt but leave it disabled until uhc_enable(). */
	ret = esp_intr_alloc(cfg->irq_source,
			     ESP_INTR_FLAG_INTRDISABLED | cfg->irq_flags,
			     uhc_dwc2_isr, (void *)dev, &data->intr_handle);
	if (ret != 0) {
		LOG_ERR("esp_intr_alloc failed: %d", ret);
		return -EIO;
	}

	k_thread_create(&data->thread_data, uhc_dwc2_thread_stack,
			K_KERNEL_STACK_SIZEOF(uhc_dwc2_thread_stack),
			uhc_dwc2_thread, (void *)dev, NULL, NULL,
			K_PRIO_COOP(2), 0, K_NO_WAIT);
	k_thread_name_set(&data->thread_data, "uhc_dwc2");

	LOG_INF("ESP32-S3 USB-OTG host controller initialised");

	return 0;
}

static struct uhc_dwc2_data uhc_dwc2_priv_0;

static struct uhc_data uhc_dwc2_data_0 = {
	.priv = &uhc_dwc2_priv_0,
};

static const struct uhc_dwc2_config uhc_dwc2_cfg_0 = {
	.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(0)),
	.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(0, offset),
	.irq_source = DT_INST_IRQN(0),
	.irq_flags = 0,
};

DEVICE_DT_INST_DEFINE(0, uhc_dwc2_driver_init, NULL,
		      &uhc_dwc2_data_0, &uhc_dwc2_cfg_0,
		      POST_KERNEL, 99, &uhc_dwc2_api);
