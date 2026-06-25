/*
 * Pro2 device init + pairing persistence.
 * Ported from upstream pro2.c: NVS -> storage abstraction, esp_fill_random ->
 * Zephyr sys_rand_get, and the controller public-address application moved into
 * the ble/ layer (the upstream esp_iface_mac_addr_set call). The value is still
 * computed/persisted here.
 */

#include "device.h"
#include "pro2.h"
#include "utils.h"
#include "codec/ns2_codec.h"
#include "storage.h"
#include "controller/controller.h"

#include <string.h>
#include <errno.h>

#include <zephyr/random/random.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(jb_pro2, CONFIG_JOY_BRIDGE_LOG_LEVEL);
#include "app_log.h"

static int pro2_addr_init(void) {
    size_t len = ESP_BD_ADDR_LEN;
    int ret = storage_get_blob(STORAGE_NS_PAIRING_PRO2, STORAGE_KEY_CTRL_ADDR,
                               g_controller_firmware.addr_re, &len);
    if (ret != 0 || len != ESP_BD_ADDR_LEN) {
        ESP_LOGW(LOG_BLE_NVS, "Pro2 addr not in storage, generating...");
        /* Randomise the lower 3 bytes (keep the Nintendo OUI prefix). */
        sys_rand_get(g_controller_firmware.addr + 3, 3);

        reverse_bytes(g_controller_firmware.addr, g_controller_firmware.addr_re,
                      ESP_BD_ADDR_LEN);
        ret = storage_set_blob(STORAGE_NS_PAIRING_PRO2, STORAGE_KEY_CTRL_ADDR,
                               g_controller_firmware.addr_re, ESP_BD_ADDR_LEN);
        if (ret != 0) {
            ESP_LOGE(LOG_BLE_NVS, "Failed to save Pro2 addr");
            return ret;
        }
    } else {
        reverse_bytes(g_controller_firmware.addr_re, g_controller_firmware.addr,
                      ESP_BD_ADDR_LEN);
        ESP_LOGI(LOG_BLE_NVS, "Pro2 addr loaded from storage");
    }
    return 0;
}

static int pro2_ltk_init(void) {
    size_t len = LTK_KEY_SIZE;
    int ret = storage_get_blob(STORAGE_NS_PAIRING_PRO2, STORAGE_KEY_LTK,
                               g_controller_firmware.ltk, &len);
    if (ret != 0 || len != LTK_KEY_SIZE) {
        ESP_LOGE(LOG_BLE_NVS, "LTK not in storage");
        return -ENOENT;
    }
    reverse_bytes(g_controller_firmware.ltk, g_controller_firmware.ltk_re, LTK_KEY_SIZE);
    ESP_LOGI(LOG_BLE_NVS, "LTK loaded and reversed successfully");
    return 0;
}

int pro2_device_init(void) {
    int ret = pro2_addr_init();
    if (ret != 0) {
        ESP_LOGW(LOG_BLE_NVS, "Pro2 addr init returned %d", ret);
    }
    /* NOTE: the actual public identity is applied to the host stack by the
     * ble/ layer (ble_apply_identity()) during ble_stack_init(), using
     * g_controller_firmware.addr computed above. */

    int ltk_ret = pro2_ltk_init();
    if (ltk_ret == 0) {
        ESP_LOGI(LOG_APP, "device already paired.");
        log_print_ltk_hex("LTK", g_controller_firmware.ltk);
    }

    const uint8_t version[3] = {0x01, 0x06, 0x01};
    const uint8_t body_color[3] = {0x23, 0x23, 0x23};
    const uint8_t buttons_color[3] = {0x63, 0xB9, 0x7A}; /* pokemon za green */
    const uint8_t highlight_color[3] = {0xE6, 0xE6, 0xE6};
    const uint8_t grip_color[3] = {0x32, 0x32, 0x32};
    set_controller_specific(0x2069,
                            (const uint8_t *)"HEJ71001123456", 14,
                            version, body_color, buttons_color,
                            highlight_color, grip_color);
    g_controller_firmware.manufacturer_data[7] = 0x69;
    g_controller_firmware.manufacturer_data[8] = 0x20;

    /* Mirror upstream: device_info_init() succeeds only when a peer (host) addr
     * was already stored, signalled here via the LTK presence. */
    return ltk_ret;
}

int pro2_pairing_info_save(void) {
    if (g_controller_firmware.type != CONTROLLER_TYPE_PRO2) {
        return 0;
    }
#ifdef CONFIG_SAVE_PAIRING_INFO
    int ret = storage_set_blob(STORAGE_NS_PAIRING_PRO2, STORAGE_KEY_LTK,
                               g_controller_firmware.ltk, LTK_KEY_SIZE);
    if (ret != 0) {
        ESP_LOGE(LOG_BLE_NVS, "Failed to save LTK");
        return ret;
    }
    ret = storage_set_blob(STORAGE_NS_PAIRING_PRO2, STORAGE_KEY_HOST_ADDR,
                           g_console_ns2.ble_addr.val, ESP_BD_ADDR_LEN);
    if (ret != 0) {
        ESP_LOGE(LOG_BLE_NVS, "Failed to save NS2 addr");
    }
    return ret;
#else
    return 0;
#endif
}

int pro2_pairing_info_erase(void) {
    if (g_controller_firmware.type != CONTROLLER_TYPE_PRO2) {
        return 0;
    }
#ifdef CONFIG_SAVE_PAIRING_INFO
    return storage_erase_namespace(STORAGE_NS_PAIRING_PRO2);
#else
    return 0;
#endif
}
