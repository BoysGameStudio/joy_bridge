/*
 * Native ESP32-S3 USB-OTG host controller (UHC) driver — private structures.
 *
 * Upstream Zephyr has no DWC2 host driver; this in-app driver implements the
 * Zephyr UHC API over the vendored Espressif host USB HAL (usb_dwc_hal) and the
 * USB PHY helper (usb_phy). Full-Speed only (the S3 OTG core is FS).
 */

#ifndef UHC_DWC2_ESP32_H
#define UHC_DWC2_ESP32_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>

#include <esp_intr_alloc.h>
#include <esp_private/usb_phy.h>
#include <hal/usb_dwc_hal.h>

/* The S3 DWC OTG core exposes up to 8 host channels. */
#define UHC_DWC2_MAX_CHANNELS 8

/* Delay after usb_dwc_hal_init before host mode takes effect (HAL contract:
 * ">=25 ms"; ESP-IDF uses INIT_DELAY_MS=30). Without it the port won't enable
 * after a bus reset. */
#define UHC_DWC2_INIT_DELAY_MS 30

/* Device-connect debounce. ESP-IDF uses 250 ms
 * (CONFIG_USB_HOST_DEBOUNCE_DELAY_MS); a too-short debounce resets the device
 * before it is stably attached and the port never enables. */
#define UHC_DWC2_DEBOUNCE_MS 250

/* Bus-reset assertion time (spec: >= 10 ms; 30 ms matches the ESP-IDF host). */
#define UHC_DWC2_RESET_HOLD_MS 30

/* Forward declaration (full definition in <zephyr/drivers/usb/uhc.h>). */
struct uhc_transfer;

struct uhc_dwc2_config {
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	int irq_source;
	int irq_flags;
};

struct uhc_dwc2_data {
	const struct device *dev;
	usb_dwc_hal_context_t hal;
	usb_phy_handle_t phy;
	intr_handle_t intr_handle;
	/* Channel handle table required by usb_dwc_hal (hal.channels.hdls). */
	usb_dwc_hal_chan_t *chan_hdls[UHC_DWC2_MAX_CHANNELS];
	struct k_thread thread_data;
	struct k_sem evt_sem;

	/* Transfer engine: one channel processes one transfer at a time (the
	 * single-active model the UHC layer is built around — cf. uhc_max3421e). */
	usb_dwc_hal_chan_t chan;
	bool chan_allocated;
	struct uhc_transfer *active;
	uint32_t in_pid; /* saved DATA toggle for the active non-control endpoint */
	/* Last channel target; the channel is only reconfigured (set_ep_char, toggle
	 * reset) when this changes, so a periodic interrupt endpoint keeps its place
	 * in the frame list and its DATA toggle across re-polls. */
	uint8_t cur_ep;
	uint8_t cur_addr;
	uint16_t cur_mps;
	/* Silent retry count for the active control transfer — rides out transient
	 * transaction errors (e.g. marginal-VBUS glitches that otherwise fail a
	 * config-descriptor read during enumeration). Reset per scheduled transfer. */
	uint8_t retries;
};

#endif /* UHC_DWC2_ESP32_H */
