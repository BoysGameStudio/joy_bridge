/* Hex/address logging helpers (ported from upstream log.c). */

#include "device.h"
#include "utils.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(jb_log, CONFIG_JOY_BRIDGE_LOG_LEVEL);
#include "app_log.h"

void log_print_ltk_hex(const char *data_name, const uint8_t *data) {
#ifdef CONFIG_MCU_DEBUG
    ESP_LOGI(LOG_APP,
             "%s: %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
             data_name, data[0], data[1], data[2], data[3], data[4], data[5],
             data[6], data[7], data[8], data[9], data[10], data[11], data[12],
             data[13], data[14], data[15]);
#else
    /* LTK is secret link-key material: never emit it at INFO in a normal build.
     * Matches the CONFIG_MCU_DEBUG gate on cmd_0x03's LTK dump (ns2_codec.c). */
    (void)data_name;
    (void)data;
#endif
}

void log_print_addr(const void *addr) {
    const uint8_t *mac = addr;
    ESP_LOGI(LOG_APP, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
}
