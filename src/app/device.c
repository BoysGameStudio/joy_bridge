/*
 * Portable device state. The BLE-specific responsibilities that lived in the
 * upstream device.c (advertising, GATT store, subscription map) now live under
 * src/ble/. Only the host-stack-agnostic lifecycle state remains here.
 */

#include "device.h"

device_status_t g_device_status = DEV_BOOT;
uint8_t g_adv_opcode = 0x00;

void device_status_set(device_status_t status) {
    g_device_status = status;
}
