/* NS2 console connection state (ported from upstream ns2.c). */

#include "console.h"

console_ns2_t g_console_ns2 = {
    .ble_addr = {
        .type = 0,          /* public */
        .val = { 0 },
    },
    .conn_handle = 0,
    .mtu = 512,
};
