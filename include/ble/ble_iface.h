#ifndef _BLE_BLE_IFACE_H_
#define _BLE_BLE_IFACE_H_

/*
 * Internal BLE interface.
 *
 * This is the boundary between the portable core (controller / codec /
 * protocol) and the Zephyr Bluetooth glue in src/ble/. Portable code includes
 * this header instead of the Zephyr bluetooth headers so it never depends on
 * the host stack directly.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Device lifecycle state ------------------------------------------------ */

typedef enum DEVICE_STATUS {
    DEV_BOOT,       /* powered on, BLE not advertising yet */
    DEV_ADV_IND,    /* advertising */
    DEV_READY,      /* paired + features enabled, HID reports may flow */
} device_status_t;

extern device_status_t g_device_status;
void device_status_set(device_status_t status);

/* Pro2 advertising opcode: 0x00 normal, 0x81 wake/reconnect. */
extern uint8_t g_adv_opcode;

/* ---- BLE stack lifecycle --------------------------------------------------- */

/* Bring up the Zephyr Bluetooth host, register the GATT service and start
 * advertising. Returns 0 on success. */
int ble_stack_init(void);

/* (Re)start advertising with the current manufacturer data. */
void ble_advertise(void);

/* ---- HID input report path ------------------------------------------------- */

/* True when a console is connected, the HID input-report characteristic
 * (upstream handle 0x000e) is subscribed, and the device is DEV_READY. */
bool ble_hid_ready(void);

/* Notify the HID input-report characteristic with one report.
 * Returns 0 on success, -ENOTCONN if no connection, or another negative errno
 * (e.g. -ENOMEM) on transient buffer exhaustion. */
int ble_hid_notify(const uint8_t *data, size_t len);

/* ---- Bonding / pairing ----------------------------------------------------- */

/* Inject the pre-derived LTK + peer address into the host's bonding context so
 * an already-paired console can reconnect without a fresh pairing exchange.
 * Returns 0 on success. (Name kept from upstream for call-site parity.) */
int inject_pairing_info_to_ble_ctx(void);

#ifdef __cplusplus
}
#endif

#endif /* _BLE_BLE_IFACE_H_ */
