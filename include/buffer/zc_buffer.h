#ifndef ZC_BUFFER_H
#define ZC_BUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP32-S3 data-cache line size (false-sharing protection between the
 * single-producer head and single-consumer tail). */
#define ZC_CACHE_LINE_SIZE 32

typedef struct {
    struct {
        volatile uint32_t head;  /* writer index */
        uint32_t watermark;      /* flow-control watermark */
        uint8_t _padding[ZC_CACHE_LINE_SIZE - sizeof(uint32_t) * 2];
    } tx __attribute__((aligned(ZC_CACHE_LINE_SIZE)));

    struct {
        volatile uint32_t tail;  /* reader index */
        uint32_t last_commit;    /* last committed length (debug) */
        uint8_t _padding[ZC_CACHE_LINE_SIZE - sizeof(uint32_t) * 2];
    } rx __attribute__((aligned(ZC_CACHE_LINE_SIZE)));

    uint8_t *buffer_base;
    uint32_t capacity;
} zc_ringbuf_t;

_Static_assert(offsetof(zc_ringbuf_t, rx) - offsetof(zc_ringbuf_t, tx) >= ZC_CACHE_LINE_SIZE,
               "False Sharing protection failed: head/tail in same cache line");
_Static_assert(sizeof(zc_ringbuf_t) % ZC_CACHE_LINE_SIZE == 0,
               "zc_ringbuf_t size must be multiple of cache line");

/* Producer: acquire contiguous writable space. */
uint32_t zc_reserve(zc_ringbuf_t *rb, uint8_t **ptr);
/* Producer: commit written bytes. */
void zc_commit(zc_ringbuf_t *rb, uint32_t len);
/* Consumer: peek readable data without consuming. */
uint32_t zc_peek(zc_ringbuf_t *rb, uint8_t **ptr);
/* Consumer: release consumed space. */
void zc_consume(zc_ringbuf_t *rb, uint32_t len);
/* Consumer: read a single byte. */
bool zc_read_byte(zc_ringbuf_t *rb, uint8_t *out);
/* Consumer: peek across the wrap boundary (two segments). */
uint32_t zc_peek_bulk(zc_ringbuf_t *rb, uint32_t wanted_len,
                      uint8_t **head_ptr, uint32_t *head_len,
                      uint8_t **wrap_ptr, uint32_t *wrap_len);
/* Consumer: copy out up to len bytes and consume them. */
uint32_t zc_read_bulk(zc_ringbuf_t *rb, uint8_t *dst, uint32_t len);

/* Init (capacity must be a power of two). Returns 0 on success, -EINVAL else. */
int  zc_init(zc_ringbuf_t *rb, uint8_t *buffer, uint32_t capacity, uint32_t watermark);
void zc_reset(zc_ringbuf_t *rb);

#ifdef __cplusplus
}
#endif

#endif /* ZC_BUFFER_H */
