/*
 * UART transport backend (Zephyr) — skeleton.
 *
 * Interrupt-driven RX drains the UART FIFO straight into the transport's
 * zero-copy ring buffer; TX is a simple poll-out. The device is the
 * "zephyr,joy-bridge-uart" chosen node if defined, else the console UART.
 *
 * TODO: dedicate a non-console UART via an overlay + chosen node
 * (zephyr,joy-bridge-uart) so logging and host input do not share a port;
 * optionally move TX to async/IRQ-driven to make submit_tx truly non-blocking.
 */

#include "transport/transport_uart.h"
#include "buffer/zc_buffer.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(jb_transport_uart, CONFIG_JOY_BRIDGE_LOG_LEVEL);
#include "app_log.h"

#if DT_HAS_CHOSEN(zephyr_joy_bridge_uart)
#define JB_UART_NODE DT_CHOSEN(zephyr_joy_bridge_uart)
#else
#define JB_UART_NODE DT_CHOSEN(zephyr_console)
#endif

static const struct device *const uart_dev = DEVICE_DT_GET(JB_UART_NODE);
static zc_ringbuf_t *s_rb;

static void uart_isr(const struct device *dev, void *user_data) {
    ARG_UNUSED(user_data);

    uart_irq_update(dev);
    while (uart_irq_rx_ready(dev)) {
        uint8_t *dst = NULL;
        uint32_t space = s_rb ? zc_reserve(s_rb, &dst) : 0;
        if (space == 0 || dst == NULL) {
            /* No room: drain and drop to keep the FIFO from stalling. */
            uint8_t scratch[16];
            (void)uart_fifo_read(dev, scratch, sizeof(scratch));
            continue;
        }
        int n = uart_fifo_read(dev, dst, space);
        if (n > 0) {
            zc_commit(s_rb, (uint32_t)n);
        }
    }
}

static int uart_open(void *instance, void *config) {
    ARG_UNUSED(instance);
    transport_uart_config_t *cfg = (transport_uart_config_t *)config;

    if (!device_is_ready(uart_dev)) {
        ESP_LOGE(LOG_TRANSPORT, "UART device not ready");
        return -1;
    }

    if (cfg && cfg->baud_rate) {
        struct uart_config uc;
        if (uart_config_get(uart_dev, &uc) == 0) {
            uc.baudrate = cfg->baud_rate;
            int rc = uart_configure(uart_dev, &uc);
            if (rc != 0 && rc != -ENOSYS) {
                ESP_LOGW(LOG_TRANSPORT, "uart_configure(%u) failed: %d", cfg->baud_rate, rc);
            }
        }
    }
    return 0;
}

static void uart_close(void *instance) {
    ARG_UNUSED(instance);
    uart_irq_rx_disable(uart_dev);
    s_rb = NULL;
}

static int uart_activate_rx(void *instance, zc_ringbuf_t *rb) {
    ARG_UNUSED(instance);
    s_rb = rb;
    uart_irq_callback_set(uart_dev, uart_isr);
    uart_irq_rx_enable(uart_dev);
    return 0;
}

static int uart_deactivate_rx(void *instance) {
    ARG_UNUSED(instance);
    uart_irq_rx_disable(uart_dev);
    return 0;
}

static int uart_submit_tx(void *instance, const uint8_t *data, uint32_t len) {
    ARG_UNUSED(instance);
    for (uint32_t i = 0; i < len; i++) {
        uart_poll_out(uart_dev, data[i]);
    }
    return 0;
}

static int uart_flush_tx(void *instance, uint32_t timeout_ms) {
    ARG_UNUSED(instance);
    ARG_UNUSED(timeout_ms);
    return 0; /* poll_out is synchronous */
}

static bool uart_is_ready(void *instance) {
    ARG_UNUSED(instance);
    return device_is_ready(uart_dev);
}

const transport_vtable_t transport_uart_vtable = {
    .name          = "uart",
    .open          = uart_open,
    .close         = uart_close,
    .activate_rx   = uart_activate_rx,
    .deactivate_rx = uart_deactivate_rx,
    .submit_tx     = uart_submit_tx,
    .flush_tx      = uart_flush_tx,
    .is_ready      = uart_is_ready,
};
