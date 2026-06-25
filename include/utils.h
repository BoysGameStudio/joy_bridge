#ifndef _UTILS_H_
#define _UTILS_H_

#include <stdint.h>
#include <stddef.h>

/* Reverse the order of bytes in an array. */
void reverse_bytes(const uint8_t *in, uint8_t *out, size_t len);

/* Encrypt one 16-byte block using AES-128 in ECB mode. Returns 0 on success. */
int aes128_ecb(const uint8_t *key, const uint8_t *in, uint8_t *out);

/* Print a 16-byte key (hex). */
void log_print_ltk_hex(const char *data_name, const uint8_t *data);

/* Print a 6-byte BLE address (stored little-endian). */
void log_print_addr(const void *addr);

/*
 * Peek a byte from a two-segment (head + wrap-around) buffer.
 * Returns the byte at index idx, or 0 if out of bounds.
 */
uint8_t peek_byte(uint8_t *head, uint32_t head_len,
                  uint8_t *wrap, uint32_t wrap_len,
                  uint32_t idx);

#endif /* _UTILS_H_ */
