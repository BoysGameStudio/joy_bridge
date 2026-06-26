/*
 * joy_bridge — application entry point.
 * Zephyr port of the upstream ESP-IDF app_main() init sequence.
 */

#include <zephyr/kernel.h>

#include "device.h"
#include "controller/hid_controller.h"
#include "storage.h"
#include "usb_host.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(jb_main, CONFIG_JOY_BRIDGE_LOG_LEVEL);
#include "app_log.h"

int main(void) {
    ESP_LOGI(LOG_APP, "Nintendo Switch Controller Emulator (joy_bridge)");
    ESP_LOGI(LOG_APP, "Starting initialization...");

    /* Persistent storage (settings) for controller config + pairing info. */
    if (storage_init() != 0) {
        ESP_LOGE(LOG_APP, "Storage init failed; continuing without persistence");
    }

    /* Controller type + firmware/pairing data from storage. */
    controller_type_init();
    if (controller_init() == 0) {
        ESP_LOGI(LOG_APP, "Controller firmware loaded (paired)");
    } else {
        ESP_LOGW(LOG_APP, "No pairing info; will pair on first connection");
    }

    /* HID report engine (double buffer + report thread, idle until subscribed). */
    if (g_hid_controller.ops->init(&g_hid_controller, g_controller_firmware.type) == 0) {
        ESP_LOGI(LOG_APP, "Controller initialized");
    } else {
        ESP_LOGE(LOG_APP, "Failed to initialize controller");
    }

    /* BLE stack: GATT service, identity, advertising. */
    if (ble_stack_init() != 0) {
        ESP_LOGE(LOG_APP, "BLE stack init failed");
    } else {
        ESP_LOGI(LOG_APP, "BLE stack initialized");
    }

#ifdef CONFIG_JOY_BRIDGE_USB_XINPUT
    /* USB host (XInput controller input). Started after BLE — the load-bearing
     * path comes up first, and a USB failure must never block it. */
    if (xinput_host_start() == 0) {
        ESP_LOGI(LOG_APP, "USB host (XInput) started");
    } else {
        ESP_LOGE(LOG_APP, "USB host (XInput) start failed; continuing without host input");
    }
#endif

    /* Zephyr keeps the kernel + worker threads running after main returns. */
    return 0;
}
