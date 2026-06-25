#ifndef TRANSPORT_H
#define TRANSPORT_H

#include "buffer/zc_buffer.h"

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_TRANSPORT "transport_layer"

typedef struct {
    const char *name;
    int  (*open)(void *instance, void *config);
    void (*close)(void *instance);

    /** Bind the RX ring buffer and start receiving. */
    int  (*activate_rx)(void *instance, zc_ringbuf_t *rb);
    /** Pause RX (flow control). */
    int  (*deactivate_rx)(void *instance);
    /** Non-blocking send. */
    int  (*submit_tx)(void *instance, const uint8_t *data, uint32_t len);
    /** Wait for TX completion. */
    int  (*flush_tx)(void *instance, uint32_t timeout_ms);
    /** Whether TX is ready. */
    bool (*is_ready)(void *instance);
} transport_vtable_t;

typedef struct {
    const transport_vtable_t *ops;
    void         *hardware_ctx;
    zc_ringbuf_t *rx_buffer;

    uint32_t tx_seq;
    uint32_t rx_seq;
    uint64_t stats_tx_bytes;
    uint64_t stats_rx_bytes;
    uint64_t stats_rx_overflow;
} transport_handle_t;

int transport_init(void);
int transport_start(void);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_H */
