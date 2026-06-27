#ifndef _APP_LOG_H_
#define _APP_LOG_H_

/*
 * ESP-IDF -> Zephyr logging shim.
 *
 * The ported sources use ESP_LOGx(TAG, fmt, ...). Zephyr selects the log module
 * per translation unit, so the TAG argument is unused here (it is still defined
 * as a string literal elsewhere, e.g. LOG_APP/LOG_HID, and simply discarded by
 * these macros).
 *
 * LOG modules are grouped by subsystem (jb_app, jb_ble, jb_codec, jb_ctrl,
 * jb_storage, jb_usb). Before including this header, the subsystem's primary .c
 * registers the module and the other .c files in that subsystem declare it:
 *
 *     #include <zephyr/logging/log.h>
 *     LOG_MODULE_REGISTER(jb_<subsys>, CONFIG_JOY_BRIDGE_LOG_LEVEL);  // once per subsystem
 *     // ...or, in the subsystem's other files:
 *     LOG_MODULE_DECLARE(jb_<subsys>, CONFIG_JOY_BRIDGE_LOG_LEVEL);
 *     #include "app_log.h"
 */

#include <zephyr/logging/log.h>

#define ESP_LOGE(tag, ...) LOG_ERR(__VA_ARGS__)
#define ESP_LOGW(tag, ...) LOG_WRN(__VA_ARGS__)
#define ESP_LOGI(tag, ...) LOG_INF(__VA_ARGS__)
#define ESP_LOGD(tag, ...) LOG_DBG(__VA_ARGS__)

#endif /* _APP_LOG_H_ */
