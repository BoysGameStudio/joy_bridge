#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

#include "buffer/zc_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_MAX_PROTOCOL_PARSERS
  #define MAX_PROTOCOL_PARSERS CONFIG_MAX_PROTOCOL_PARSERS
#else
  #define MAX_PROTOCOL_PARSERS 16
#endif

#define LOG_PROTOCOL "protocol_layer"

typedef enum {
    PARSE_OK = 0,
    PARSE_NEED_MORE,
    PARSE_INVALID,
    PARSE_OVERFLOW,
} parse_result_t;

typedef struct {
    uint8_t *data;
    uint32_t len;
} parser_rsp_t;

/** @brief Operations table for an individual protocol parser. */
typedef struct {
    const char *name;
    uint32_t min_peek_len;
    void (*reset)(void *state);
    bool (*probe)(void *state,
                  uint8_t *head_ptr, uint32_t head_len,
                  uint8_t *wrap_ptr, uint32_t wrap_len);
    parse_result_t (*parse_frame)(void *state,
                                  zc_ringbuf_t *rb,
                                  parser_rsp_t *rsp);
} protocol_parser_ops_t;

typedef struct {
    const protocol_parser_ops_t *ops;
    void *state;
} protocol_parser_t;

typedef struct {
    const char *name;
    uint32_t max_peek_len;
    uint32_t parser_count;
    protocol_parser_t *parsers[MAX_PROTOCOL_PARSERS];
} protocol_instance_t;

/**
 * @brief Route incoming data to the first matching parser.
 * @return PARSE_OK on success, PARSE_NEED_MORE if more data is required,
 *         PARSE_INVALID if no parser matched.
 */
parse_result_t protocol_route(protocol_instance_t *inst,
                              zc_ringbuf_t *rb,
                              parser_rsp_t *rsp);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_H */
