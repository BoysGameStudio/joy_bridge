#ifndef _STORAGE_H_
#define _STORAGE_H_

/*
 * Tiny key/value persistence abstraction, replacing the upstream ESP-IDF NVS
 * usage. Backed by the Zephyr settings subsystem (see src/storage/storage.c).
 *
 * Keys are namespaced (namespace + key) the same way the original code grouped
 * values into NVS namespaces (CONFIG / PAIRING_PRO2 / ...).
 */

#include <stdint.h>
#include <stddef.h>

/* Namespaces (mirror upstream NVS namespace names). */
#define STORAGE_NS_CONFIG        "config"
#define STORAGE_NS_PAIRING_PRO2  "pair_pro2"
#define STORAGE_NS_PAIRING_JC    "pair_jc"

/* Keys (mirror upstream NVS keys). */
#define STORAGE_KEY_CONTROLLER_TYPE  "ctrl_type"
#define STORAGE_KEY_CTRL_ADDR        "ctrl_mac"
#define STORAGE_KEY_HOST_ADDR        "host_mac"
#define STORAGE_KEY_LTK              "ble_ltk"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the storage backend. Returns 0 on success. */
int storage_init(void);

/* Read a blob. On entry *len is the buffer capacity; on success it holds the
 * bytes read. Returns 0 on success, -ENOENT if absent, other negative errno
 * on error. */
int storage_get_blob(const char *ns, const char *key, void *out, size_t *len);

/* Write a blob. Returns 0 on success. */
int storage_set_blob(const char *ns, const char *key, const void *in, size_t len);

/* Read / write a single byte. */
int storage_get_u8(const char *ns, const char *key, uint8_t *out);
int storage_set_u8(const char *ns, const char *key, uint8_t val);

/* Erase every key under a namespace. Returns 0 on success. */
int storage_erase_namespace(const char *ns);

#ifdef __cplusplus
}
#endif

#endif /* _STORAGE_H_ */
