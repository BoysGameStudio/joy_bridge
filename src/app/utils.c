/*
 * Small helpers. Ported from upstream utils.c; the ESP-IDF/mbedTLS AES path is
 * replaced with Zephyr's bt_encrypt_be() — standard big-endian AES-128-ECB,
 * which matches the upstream mbedTLS/esp_aes block semantics and is always
 * available once Bluetooth is enabled (no extra crypto Kconfig needed).
 */

#include "utils.h"

#include <string.h>

#include <zephyr/bluetooth/crypto.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(jb_app, CONFIG_JOY_BRIDGE_LOG_LEVEL);
#include "app_log.h"
#include "device.h"

void reverse_bytes(const uint8_t *in, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        out[i] = in[len - 1 - i];
    }
}

/*
 * AES-128 ECB single-block encrypt. Used by the NS2 pairing LTK-confirm step
 * (cmd 0x15 / subcmd 0x02). bt_encrypt_be() is standard big-endian AES-128,
 * matching upstream. Returns 0 on success.
 */
int aes128_ecb(const uint8_t *key, const uint8_t *in, uint8_t *out) {
    int rc = bt_encrypt_be(key, in, out);
    if (rc != 0) {
        ESP_LOGE(LOG_APP, "bt_encrypt_be failed: %d", rc);
    }
    return rc;
}
