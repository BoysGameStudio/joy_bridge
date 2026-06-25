#ifndef TRANSPORT_UART_H
#define TRANSPORT_UART_H

#include "transport/transport.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief UART transport configuration (Zephyr).
 *
 * The device is resolved from the devicetree (see src/transport/transport_uart.c):
 * the "zephyr,joy-bridge-uart" chosen node if present, otherwise the console
 * UART. baud_rate may reconfigure the port at open().
 */
typedef struct {
    uint32_t baud_rate;
    int      rx_buffer_size;   /* informational; RX flows into the zc ring buffer */
    int      tx_buffer_size;   /* informational */
} transport_uart_config_t;

extern const transport_vtable_t transport_uart_vtable;

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_UART_H */
