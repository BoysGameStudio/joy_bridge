/*
 * Single-producer / single-consumer zero-copy ring buffer.
 * Ported from the upstream ESP-IDF implementation: ESP-IDF atomics/attrs
 * replaced with GCC __atomic builtins (portable on Zephyr), esp_err_t with
 * plain int/errno, and ESP_LOG with the Zephyr log shim.
 */

#include "buffer/zc_buffer.h"

#include <string.h>
#include <errno.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(jb_zc_buffer, CONFIG_JOY_BRIDGE_LOG_LEVEL);
#include "app_log.h"

#define TAG "zc_buffer"

static inline uint32_t _zc_used_space(uint32_t head, uint32_t tail, uint32_t cap) {
    if (head >= tail) return head - tail;
    return cap - (tail - head); /* wrap-around */
}

static inline uint32_t _zc_free_space(uint32_t head, uint32_t tail, uint32_t cap) {
    /* Reserve 1 byte to distinguish full from empty. */
    return cap - 1 - _zc_used_space(head, tail, cap);
}

#define _zc_memory_barrier()   __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define _zc_compiler_barrier()  __atomic_signal_fence(__ATOMIC_SEQ_CST)

#define _zc_load(p, order)      __atomic_load_n((p), (order))
#define _zc_store(p, v, order)  __atomic_store_n((p), (v), (order))

uint32_t zc_reserve(zc_ringbuf_t *rb, uint8_t **ptr) {
    if (!rb || !ptr) return 0;

    uint32_t head = _zc_load(&rb->tx.head, __ATOMIC_ACQUIRE);
    uint32_t tail = _zc_load(&rb->rx.tail, __ATOMIC_RELAXED);

    uint32_t free = _zc_free_space(head, tail, rb->capacity);
    if (free == 0) {
        /* re-read tail in case the consumer just advanced */
        tail = _zc_load(&rb->rx.tail, __ATOMIC_ACQUIRE);
        free = _zc_free_space(head, tail, rb->capacity);
    }
    if (free == 0) return 0;

    uint32_t pos = head & (rb->capacity - 1);
    uint32_t to_end = rb->capacity - pos;
    uint32_t avail = (free < to_end) ? free : to_end;
    if (avail == 0) return 0;

    *ptr = rb->buffer_base + pos;
    _zc_compiler_barrier();
    return avail;
}

void zc_commit(zc_ringbuf_t *rb, uint32_t len) {
    if (!rb || len == 0) return;

    _zc_memory_barrier();

    uint32_t old_head = _zc_load(&rb->tx.head, __ATOMIC_RELAXED);
    uint32_t new_head = (old_head + len) & (rb->capacity - 1);
    _zc_store(&rb->tx.head, new_head, __ATOMIC_RELEASE);

    rb->rx.last_commit = len;
}

uint32_t zc_peek(zc_ringbuf_t *rb, uint8_t **ptr) {
    if (!rb || !ptr) return 0;

    uint32_t tail = _zc_load(&rb->rx.tail, __ATOMIC_ACQUIRE);
    uint32_t head = _zc_load(&rb->tx.head, __ATOMIC_RELAXED);

    uint32_t used = _zc_used_space(head, tail, rb->capacity);
    if (used == 0) return 0;

    uint32_t pos = tail & (rb->capacity - 1);
    uint32_t to_end = rb->capacity - pos;
    uint32_t avail = (used < to_end) ? used : to_end;
    if (avail == 0) return 0;

    *ptr = rb->buffer_base + pos;
    _zc_compiler_barrier();
    return avail;
}

void zc_consume(zc_ringbuf_t *rb, uint32_t len) {
    if (!rb || len == 0) return;

    _zc_memory_barrier();

    uint32_t old_tail = _zc_load(&rb->rx.tail, __ATOMIC_RELAXED);
    uint32_t new_tail = (old_tail + len) & (rb->capacity - 1);
    _zc_store(&rb->rx.tail, new_tail, __ATOMIC_RELEASE);
}

bool zc_read_byte(zc_ringbuf_t *rb, uint8_t *out) {
    if (!rb || !out) return false;

    uint32_t tail = _zc_load(&rb->rx.tail, __ATOMIC_ACQUIRE);
    uint32_t head = _zc_load(&rb->tx.head, __ATOMIC_RELAXED);
    if (tail == head) return false;

    uint32_t pos = tail & (rb->capacity - 1);
    *out = rb->buffer_base[pos];

    _zc_compiler_barrier();
    uint32_t new_tail = (tail + 1) & (rb->capacity - 1);
    _zc_store(&rb->rx.tail, new_tail, __ATOMIC_RELEASE);
    return true;
}

uint32_t zc_peek_bulk(zc_ringbuf_t *rb, uint32_t wanted_len,
                      uint8_t **head_ptr, uint32_t *head_len,
                      uint8_t **wrap_ptr, uint32_t *wrap_len) {
    if (!rb || !head_ptr || !head_len) return 0;

    *head_ptr = NULL;
    *head_len = 0;
    if (wrap_ptr) *wrap_ptr = NULL;
    if (wrap_len) *wrap_len = 0;
    if (wanted_len == 0) return 0;

    uint32_t tail = _zc_load(&rb->rx.tail, __ATOMIC_ACQUIRE);
    uint32_t head = _zc_load(&rb->tx.head, __ATOMIC_RELAXED);

    uint32_t used = _zc_used_space(head, tail, rb->capacity);
    if (used == 0) return 0;

    uint32_t to_peek = (wanted_len < used) ? wanted_len : used;
    uint32_t pos = tail & (rb->capacity - 1);
    uint32_t to_end = rb->capacity - pos;

    if (to_peek <= to_end) {
        *head_ptr = rb->buffer_base + pos;
        *head_len = to_peek;
    } else {
        *head_ptr = rb->buffer_base + pos;
        *head_len = to_end;
        if (wrap_ptr) *wrap_ptr = rb->buffer_base;
        if (wrap_len) *wrap_len = to_peek - to_end;
    }

    _zc_compiler_barrier();
    return to_peek;
}

uint32_t zc_read_bulk(zc_ringbuf_t *rb, uint8_t *dst, uint32_t len) {
    if (!rb || !dst || len == 0) return 0;

    uint8_t *head_ptr, *wrap_ptr;
    uint32_t head_len, wrap_len;
    uint32_t avail = zc_peek_bulk(rb, len, &head_ptr, &head_len, &wrap_ptr, &wrap_len);
    if (avail == 0) return 0;

    uint32_t to_copy = (len < avail) ? len : avail;
    if (to_copy <= head_len) {
        memcpy(dst, head_ptr, to_copy);
    } else {
        memcpy(dst, head_ptr, head_len);
        if (wrap_ptr && to_copy > head_len) {
            memcpy(dst + head_len, wrap_ptr, to_copy - head_len);
        }
    }

    zc_consume(rb, to_copy);
    return to_copy;
}

int zc_init(zc_ringbuf_t *rb, uint8_t *buffer, uint32_t capacity, uint32_t watermark) {
    if (!rb || !buffer) return -EINVAL;

    if ((capacity & (capacity - 1)) != 0) {
        ESP_LOGE(TAG, "capacity must be power of 2");
        return -EINVAL;
    }

    memset(rb, 0, sizeof(zc_ringbuf_t));
    rb->buffer_base = buffer;
    rb->capacity = capacity;
    rb->tx.watermark = watermark;

    _zc_store(&rb->tx.head, 0, __ATOMIC_RELAXED);
    _zc_store(&rb->rx.tail, 0, __ATOMIC_RELAXED);
    return 0;
}

void zc_reset(zc_ringbuf_t *rb) {
    if (!rb) return;

    _zc_store(&rb->rx.tail, 0, __ATOMIC_RELEASE);
    _zc_memory_barrier();
    _zc_store(&rb->tx.head, 0, __ATOMIC_RELEASE);
    rb->rx.last_commit = 0;
}
