/*
 * Controller firmware data + type dispatch. Ported from upstream controller.c.
 * The NimBLE LTK-store injection (ltk_sec_init / inject_pairing_info_to_ble_ctx)
 * moved to src/ble/ble_store.c; NVS access moved behind the storage abstraction.
 */

#include "device.h"
#include "controller/controller.h"
#include "utils.h"
#include "pro2.h"
#include "storage.h"

#include <errno.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(jb_ctrl, CONFIG_JOY_BRIDGE_LOG_LEVEL);
#include "app_log.h"

controller_firmware_t g_controller_firmware = {
    /* 78:81:8c -> Nintendo (Area HK) */
    .addr = { 0x78, 0x81, 0x8c, 0x00, 0x00, 0x00 },
    .addr_re = { 0 },
    .ltk = { 0 },
    .ltk_re = { 0 },
    .ltk_key_b1 = {
        0x5C, 0xF6, 0xEE, 0x79, 0x2C, 0xDF, 0x05, 0xE1,
        0xBA, 0x2B, 0x63, 0x25, 0xC4, 0x1A, 0x5F, 0x10
    },
    .type = CONTROLLER_TYPE_PRO2,
    .manufacturer_data = {
        0x53, 0x05,                                 /* Manufacturer ID, Nintendo */
        0x01, 0x00, 0x03,                           /* fixed, maybe version */
        0x7E, 0x05,                                 /* Vendor Id, Nintendo */
        0x69, 0x20,                                 /* Product ID, Pro2 */
        0x00, 0x01,                                 /* fixed / placeholder */
        0x00,                                       /* 0x81 -> wake adv, 0x00 otherwise */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,         /* ns2 address, little endian */
        0x0F,                                       /* fixed */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00    /* placeholder */
    }
};

void controller_type_init(void) {
    uint8_t type = CONTROLLER_TYPE_PRO2;

    int ret = storage_get_u8(STORAGE_NS_CONFIG, STORAGE_KEY_CONTROLLER_TYPE, &type);
    if (ret != 0) {
        ESP_LOGW(LOG_BLE_NVS, "Controller type not in storage, defaulting to PRO2");
        type = CONTROLLER_TYPE_PRO2;
    } else {
        ESP_LOGI(LOG_BLE_NVS, "Controller type loaded from storage: %d", type);
    }
    g_controller_firmware.type = type;
}

int controller_init(void) {
    if (g_controller_firmware.type == CONTROLLER_TYPE_PRO2) {
        return pro2_device_init();
    }
    ESP_LOGE(LOG_APP, "JoyCon device init not implemented");
    return -ENOSYS;
}

int controller_pairing_info_save(void) {
    if (g_controller_firmware.type == CONTROLLER_TYPE_PRO2) {
        return pro2_pairing_info_save();
    }
    ESP_LOGE(LOG_APP, "JoyCon pairing info save not implemented");
    return -1;
}

int controller_pairing_info_erase(void) {
    if (g_controller_firmware.type == CONTROLLER_TYPE_PRO2) {
        return pro2_pairing_info_erase();
    }
    ESP_LOGE(LOG_APP, "JoyCon pairing info erase not implemented");
    return -1;
}
