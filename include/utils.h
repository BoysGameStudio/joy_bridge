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

#endif /* _UTILS_H_ */
