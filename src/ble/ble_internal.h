#ifndef _BLE_BLE_INTERNAL_H_
#define _BLE_BLE_INTERNAL_H_

/* Shared declarations between the src/ble/ translation units (ble.c / gatt.c /
 * ble_store.c). Not part of the portable-core interface (that is ble_iface.h). */

#include <zephyr/bluetooth/conn.h>

/* The single active console connection, owned by ble.c (NULL when idle). */
struct bt_conn *ble_active_conn(void);

/* Local BLE identity index used for advertising/bonding (owned by ble.c). */
uint8_t ble_identity(void);

/* Register the GATT service bookkeeping (cache notify attributes). Called once
 * from ble_stack_init() after bt_enable(). */
void ble_gatt_init(void);

/* Called by gatt.c when the HID input-report CCC subscription changes; lets
 * ble.c kick/stop the controller report thread (mirrors upstream gap.c). */
void ble_on_hid_subscription(bool enabled);

#endif /* _BLE_BLE_INTERNAL_H_ */
