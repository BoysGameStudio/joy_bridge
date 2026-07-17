#ifndef JOY_BRIDGE_USB_HOST_H
#define JOY_BRIDGE_USB_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialise and enable the USB host stack, then begin polling for an input
 * device on the ESP32-S3 USB-OTG port — a wired XInput controller and/or a
 * USB HID boot-protocol keyboard (whichever binds; no hub support, so never
 * both at once). Reports from a connected device are merged and mapped onto
 * the emulated Pro2 HID report.
 *
 * Returns 0 on success, negative errno on failure. A failure here must not
 * block BLE bring-up — the caller logs and continues. Only meaningful when
 * CONFIG_JOY_BRIDGE_USB_XINPUT or CONFIG_JOY_BRIDGE_USB_KBD is enabled.
 */
int usb_host_start(void);

#ifdef __cplusplus
}
#endif

#endif /* JOY_BRIDGE_USB_HOST_H */
