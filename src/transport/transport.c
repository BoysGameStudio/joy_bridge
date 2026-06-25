/*
 * Transport dispatcher — ported from upstream transport.c.
 * FreeRTOS task -> Zephyr thread; ESP-IDF UART config -> Zephyr UART config.
 * Only the UART backend is provided in this scaffold (USB CDC / USB-Serial-JTAG
 * backends from upstream are future work).
 */

#include "transport/transport.h"
#include "buffer/zc_buffer.h"
#include "protocol/protocol.h"
#include "controller/hid_controller.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(jb_transport, CONFIG_JOY_BRIDGE_LOG_LEVEL);
#include "app_log.h"

#ifdef CONFIG_TRANSPORT_LAYER_UART
#include "transport/transport_uart.h"
#endif

/* See upstream for the sizing rationale; 256 B covers normal scheduler jitter. */
#define TRANSPORT_RX_BUF_SIZE 256
#define TRANSPORT_TX_BUF_SIZE 256

#define TRANSPORT_TASK_STACK_SIZE 4096
#define TRANSPORT_TASK_PRIO       5

static transport_handle_t g_transport;
static zc_ringbuf_t       g_transport_rx_ringbuf;
static uint8_t            g_transport_rx_buffer[TRANSPORT_RX_BUF_SIZE];

static protocol_instance_t *g_protocol_inst;

K_THREAD_STACK_DEFINE(transport_task_stack, TRANSPORT_TASK_STACK_SIZE);
static struct k_thread transport_task_data;
static k_tid_t transport_task_tid;

static void transport_protocol_task(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (1) {
        if (g_protocol_inst == NULL || g_transport.ops == NULL ||
            !g_transport.ops->is_ready(&g_transport)) {
            k_msleep(100);
            continue;
        }

        parser_rsp_t rsp = {0};
        parse_result_t result = protocol_route(g_protocol_inst, &g_transport_rx_ringbuf, &rsp);

        if (result == PARSE_OK) {
            if (rsp.len > 0 && g_transport.ops->submit_tx != NULL) {
                g_transport.ops->submit_tx(&g_transport, rsp.data, rsp.len);
            }
        } else if (result == PARSE_NEED_MORE) {
            k_msleep(2);
        } else {
            /* PARSE_INVALID: drop one byte to avoid stalling on stale data. */
            uint8_t discard;
            zc_read_byte(&g_transport_rx_ringbuf, &discard);
            k_msleep(1);
        }
    }
}

int transport_init(void) {
    memset(&g_transport, 0, sizeof(g_transport));
    memset(g_transport_rx_buffer, 0, sizeof(g_transport_rx_buffer));

    if (zc_init(&g_transport_rx_ringbuf, g_transport_rx_buffer, TRANSPORT_RX_BUF_SIZE, 0) != 0) {
        ESP_LOGE(LOG_TRANSPORT, "Failed to initialize transport RX ring buffer");
        return -1;
    }

    /* No application-layer protocol is currently implemented. The UART backend
     * still drains host bytes into the RX ring; assign a protocol_instance_t to
     * g_protocol_inst here to decode them into HID reports. */
    g_protocol_inst = NULL;
    ESP_LOGI(LOG_TRANSPORT, "No application protocol wired; host input not decoded");

#ifdef CONFIG_TRANSPORT_LAYER_UART
    g_transport.ops = &transport_uart_vtable;

    transport_uart_config_t uart_cfg = {
        .baud_rate = 115200,
        .rx_buffer_size = TRANSPORT_RX_BUF_SIZE,
        .tx_buffer_size = TRANSPORT_TX_BUF_SIZE,
    };

    if (g_transport.ops->open(&g_transport, &uart_cfg) != 0) {
        ESP_LOGE(LOG_TRANSPORT, "Failed to open UART transport");
        return -1;
    }
    if (g_transport.ops->activate_rx(&g_transport, &g_transport_rx_ringbuf) != 0) {
        ESP_LOGE(LOG_TRANSPORT, "Failed to activate UART RX");
        g_transport.ops->close(&g_transport);
        return -1;
    }
    ESP_LOGI(LOG_TRANSPORT, "UART transport initialized");
#else
    ESP_LOGW(LOG_TRANSPORT, "No transport layer selected; running without host input");
    return 0;
#endif

    return 0;
}

int transport_start(void) {
    if (g_transport.ops == NULL) {
        ESP_LOGW(LOG_TRANSPORT, "No transport active; protocol task not started");
        return 0;
    }
    if (transport_task_tid != NULL) {
        ESP_LOGW(LOG_TRANSPORT, "Protocol dispatcher task already started");
        return 0;
    }

    transport_task_tid = k_thread_create(&transport_task_data, transport_task_stack,
                                         K_THREAD_STACK_SIZEOF(transport_task_stack),
                                         transport_protocol_task, NULL, NULL, NULL,
                                         TRANSPORT_TASK_PRIO, 0, K_NO_WAIT);
    if (transport_task_tid == NULL) {
        ESP_LOGE(LOG_TRANSPORT, "Failed to create protocol dispatcher task");
        return -1;
    }
    k_thread_name_set(transport_task_tid, "transport_proto");
    ESP_LOGI(LOG_TRANSPORT, "Protocol dispatcher task started");
    return 0;
}
