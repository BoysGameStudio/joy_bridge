#ifndef _CONSOLE_H_
#define _CONSOLE_H_

#include <stdint.h>

#define NS2_BD_ADDR_LEN 6

/*
 * Portable BLE address holder. Decoupled from any host-stack address type so
 * the codec/controller layers do not pull in the Zephyr bluetooth headers.
 * The ble/ layer translates to/from bt_addr_le_t.
 *
 * val[] is stored little-endian (LSB first), matching the upstream NimBLE
 * ble_addr_t convention and Zephyr's bt_addr_le_t.
 */
typedef struct {
    uint8_t type;                   /* 0 = public */
    uint8_t val[NS2_BD_ADDR_LEN];
} ns2_addr_t;

typedef struct {
    ns2_addr_t ble_addr;
    uint16_t   conn_handle;         /* legacy field; see ble layer for live conn */
    uint16_t   mtu;
} console_ns2_t;

extern console_ns2_t g_console_ns2;

#endif /* _CONSOLE_H_ */
