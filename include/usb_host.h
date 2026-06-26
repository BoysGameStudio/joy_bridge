#ifndef JOY_BRIDGE_USB_HOST_H
#define JOY_BRIDGE_USB_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialise and enable the USB host stack, then begin polling for a wired
 * XInput controller on the ESP32-S3 USB-OTG port. Reports from a connected
 * controller are mapped onto the emulated Pro2 HID report.
 *
 * Returns 0 on success, negative errno on failure. A failure here must not
 * block BLE bring-up — the caller logs and continues. Only meaningful when
 * CONFIG_JOY_BRIDGE_USB_XINPUT is enabled.
 */
int xinput_host_start(void);

#ifdef __cplusplus
}
#endif

#endif /* JOY_BRIDGE_USB_HOST_H */
