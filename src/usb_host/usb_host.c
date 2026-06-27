/*
 * USB host bring-up for the XInput controller bridge.
 *
 * Defines the USB host support context bound to the native ESP32-S3 USB-OTG
 * host controller (uhc_dwc2_esp32) and starts the host stack. The XInput class
 * driver (xinput_class.c) self-registers and claims a controller when one is
 * plugged in.
 */

#include "usb_host.h"

#include <zephyr/kernel.h>

#ifdef CONFIG_JOY_BRIDGE_USB_XINPUT

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/usb/usbh.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(jb_usb, CONFIG_JOY_BRIDGE_LOG_LEVEL);

USBH_CONTROLLER_DEFINE(xinput_uhs, DEVICE_DT_GET(DT_NODELABEL(usb_otg)));

int xinput_host_start(void)
{
	int ret;

	ret = usbh_init(&xinput_uhs);
	if (ret != 0) {
		LOG_ERR("usbh_init failed: %d", ret);
		return ret;
	}

	ret = usbh_enable(&xinput_uhs);
	if (ret != 0) {
		LOG_ERR("usbh_enable failed: %d", ret);
		return ret;
	}

	LOG_INF("USB host enabled; waiting for an XInput controller");
	return 0;
}

#endif /* CONFIG_JOY_BRIDGE_USB_XINPUT */
