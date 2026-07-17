/*
 * USB host bring-up for the input-device bridge.
 *
 * Defines the USB host support context bound to the native ESP32-S3 USB-OTG
 * host controller (uhc_dwc2_esp32) and starts the host stack. The class
 * drivers (xinput_class.c / kbd_class.c) self-register and claim an input
 * device when one is plugged in.
 */

#include "usb_host.h"

#include <zephyr/kernel.h>

#if defined(CONFIG_JOY_BRIDGE_USB_XINPUT) || defined(CONFIG_JOY_BRIDGE_USB_KBD)

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/usb/usbh.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(jb_usb, CONFIG_JOY_BRIDGE_LOG_LEVEL);

USBH_CONTROLLER_DEFINE(xinput_uhs, DEVICE_DT_GET(DT_NODELABEL(usb_otg)));

int usb_host_start(void)
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

	LOG_INF("USB host enabled; waiting for an input device (XInput pad / HID keyboard)");
	return 0;
}

#endif /* CONFIG_JOY_BRIDGE_USB_XINPUT || CONFIG_JOY_BRIDGE_USB_KBD */
