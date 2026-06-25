#ifndef _DEVICE_H_
#define _DEVICE_H_

/*
 * Central application header for the portable core. Aggregates the cross-layer
 * declarations the upstream sources expect from "device.h", but free of any
 * host-stack (NimBLE/Zephyr-bt) types — those live behind ble/ble_iface.h.
 */

#include <stdint.h>
#include <stdbool.h>

#include "console.h"
#include "controller/controller.h"
#include "ble/ble_iface.h"

/* Log "tags" — kept as string literals for call-site parity with the upstream
 * ESP_LOGx(TAG, ...) calls. The app_log.h shim discards the tag argument; the
 * active Zephyr log module is chosen per-file via LOG_MODULE_REGISTER(). */
#define LOG_APP        "app"
#define LOG_HID        "hid"
#define LOG_BLE_GATT   "ble_gatt"
#define LOG_BLE_GAP    "ble_gap"
#define LOG_BLE_NVS    "ble_nvs"

#endif /* _DEVICE_H_ */
