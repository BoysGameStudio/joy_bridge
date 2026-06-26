/*
 * BLE host glue — Zephyr port of upstream device.c (GAP) + gap.c.
 *
 * ============================ 5 ms INTERVAL NOTE ============================
 * The NS2 console drives a sub-spec ~5 ms connection interval.
 *   HOST side (solved here): CONFIG_BT_CONN_PARAM_ANY=y removes the Zephyr
 *     host's interval_min>=6 (7.5 ms) check, and le_param_req() below accepts
 *     whatever the console proposes (the analog of NimBLE's
 *     *self_params = *peer_params).
 *   CONTROLLER side (already enabled on this S3 build — verified at the binary
 *     level): the legacy btdm driver components/bt/controller/esp32c3/bt.c
 *     calls ble_min_conn_interval_enable(3) (=3.75 ms floor) in
 *     btdm_funcs_table_ready_wrapper() under CONFIG_BT_CTRL_BLE_MIN_CONN_-
 *     INTERVAL_ENABLE (hardcoded 1 in esp_bt.h). The REAL implementation is
 *     linked from the blob's arch_main.o (it stores g_ble_con_interval_min and
 *     patches the LL conn-param range check); the no-op in the HAL's
 *     port/stubs/bt_stubs.c is NOT compiled for S3. So a 3.75/5 ms interval is
 *     permitted by the controller -> nothing to re-pin or un-stub. The only
 *     open item is confirming the negotiated interval on hardware. See the
 *     project memory note "5ms-conn-interval-zephyr-s3".
 * ===========================================================================
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/settings/settings.h>

#include <esp_mac.h>

#include <string.h>
#include <errno.h>

#include "device.h"
#include "controller/controller.h"
#include "controller/hid_controller.h"
#include "utils.h"
#include "ble/ble_iface.h"
#include "ble_internal.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(jb_ble, CONFIG_JOY_BRIDGE_LOG_LEVEL);
#include "app_log.h"

static struct bt_conn *default_conn;
static uint8_t adv_id = BT_ID_DEFAULT;

/* ESP32-S3 btdm controller hooks for sub-spec connection intervals (defined in
 * the blob's arch_main.o). The driver is meant to call this from
 * btdm_funcs_table_ready_wrapper(); we also force it at runtime in case that
 * wrapper did not run, so the controller accepts NS2's ~5 ms CONNECT_IND. */
extern uint16_t g_ble_con_interval_min;
extern void ble_min_conn_interval_enable(uint16_t min_interval);

struct bt_conn *ble_active_conn(void) {
    return default_conn;
}

uint8_t ble_identity(void) {
    return adv_id;
}

/* ---- Advertising ---------------------------------------------------------- */

static void adv_restart_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    ble_advertise();
}
static K_WORK_DELAYABLE_DEFINE(adv_restart_work, adv_restart_work_fn);

#if defined(CONFIG_BT_SMP) && CONFIG_JOY_BRIDGE_BLE_FAST_CONN_INTERVAL > 0
/* Post-pairing, request a faster connection interval to cut input latency. The
 * NS2 (central) decides whether to accept; on_le_param_updated() logs the
 * negotiated value. Fired only after security level 4 (+ a settle delay) so it
 * cannot perturb the load-bearing pairing handshake. */
static uint8_t fast_interval_attempts;

/* The NS2 honours a peripheral param-update request only intermittently, so
 * retry every few seconds until the link reaches the target interval (or give
 * up after a bounded number of tries). */
static void fast_interval_work_fn(struct k_work *work) {
    struct bt_conn *conn = default_conn;
    if (conn == NULL) {
        return;
    }

    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) == 0 &&
        (info.le.interval_us / 1250U) <= CONFIG_JOY_BRIDGE_BLE_FAST_CONN_INTERVAL) {
        return; /* already at/below the requested interval — done */
    }
    if (fast_interval_attempts >= 5) {
        ESP_LOGW(LOG_BLE_GAP, "NS2 kept the slow interval after %u requests",
                 fast_interval_attempts);
        return;
    }
    fast_interval_attempts++;

    uint16_t itvl = CONFIG_JOY_BRIDGE_BLE_FAST_CONN_INTERVAL;
    struct bt_le_conn_param param = {
        .interval_min = itvl,
        .interval_max = itvl,
        .latency = 0,
        .timeout = 200, /* 2 s supervision */
    };
    bt_conn_le_param_update(conn, &param);
    ESP_LOGI(LOG_BLE_GAP, "fast conn interval request #%u: %u (%u.%02u ms)",
             fast_interval_attempts, itvl, (itvl * 5) / 4, ((itvl * 5) % 4) * 25);

    k_work_reschedule(k_work_delayable_from_work(work), K_MSEC(3000));
}
static K_WORK_DELAYABLE_DEFINE(fast_interval_work, fast_interval_work_fn);
#endif

void ble_advertise(void) {
    if (g_controller_firmware.type == CONTROLLER_TYPE_JOYCON) {
        ESP_LOGE(LOG_APP, "Joycon not implemented");
        return;
    }

    /* Apply wake/reconnect opcode + console address into the manufacturer data
     * (mirrors upstream ble_advertise()). */
    if (g_adv_opcode != 0x00) {
        g_controller_firmware.manufacturer_data[11] = g_adv_opcode;
        memcpy(&g_controller_firmware.manufacturer_data[12],
               g_console_ns2.ble_addr.val, NS2_BD_ADDR_LEN);
    }

    struct bt_data ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
        BT_DATA(BT_DATA_MANUFACTURER_DATA,
                g_controller_firmware.manufacturer_data,
                sizeof(g_controller_firmware.manufacturer_data)),
    };

    /* Legacy connectable advertising (ADV_IND) with the PUBLIC identity address.
     * Equivalent on-air to upstream's legacy-PDU ext-adv. Using the legacy API
     * to rule out any extended-adv address quirk on the ESP32 controller. */
    struct bt_le_adv_param param = {
        .id = adv_id,
        .options = BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY,
        .interval_min = 0x0030, /* 30 ms (upstream BLE_GAP_ADV_FAST_INTERVAL1_MIN) */
        .interval_max = 0x0030,
    };

    int rc = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), NULL, 0);
    if (rc == -EALREADY) {
        device_status_set(DEV_ADV_IND);
        return;
    }
    if (rc != 0) {
        ESP_LOGE(LOG_APP, "advertising start failed: %d", rc);
        return;
    }
    device_status_set(DEV_ADV_IND);

    /* Log the actual local advertising address for verification. */
    bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
    size_t count = ARRAY_SIZE(addrs);
    bt_id_get(addrs, &count);
    if (count > adv_id) {
        char s[BT_ADDR_LE_STR_LEN];
        bt_addr_le_to_str(&addrs[adv_id], s, sizeof(s));
        ESP_LOGI(LOG_APP, "advertising started (legacy ADV_IND) — AdvA=%s", s);
    } else {
        ESP_LOGI(LOG_APP, "advertising started (legacy ADV_IND)");
    }
}

/* ---- Connection callbacks ------------------------------------------------- */

static void on_connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        ESP_LOGE(LOG_BLE_GAP, "connection failed, err=%u, restart advertising", err);
        ble_advertise();
        return;
    }

    k_work_cancel_delayable(&adv_restart_work);
    default_conn = bt_conn_ref(conn);

    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) == 0 && info.role == BT_CONN_ROLE_PERIPHERAL) {
        if (g_device_status == DEV_ADV_IND) {
            g_console_ns2.ble_addr.type = info.le.dst->type;
            memcpy(g_console_ns2.ble_addr.val, info.le.dst->a.val, NS2_BD_ADDR_LEN);
            ESP_LOGI(LOG_BLE_GAP, "connected, NS2 addr:");
            log_print_addr(g_console_ns2.ble_addr.val);
            /* bt_conn_le_info.interval (1.25 ms units) is deprecated in this
             * Zephyr in favour of interval_us; derive the units value once for
             * the log line and the param-update request below. */
            uint16_t interval = (uint16_t)(info.le.interval_us / 1250U);
            ESP_LOGI(LOG_BLE_GAP, "initial conn: interval=%u (%u.%02u ms) latency=%u timeout=%u",
                     interval, (interval * 5) / 4,
                     ((interval * 5) % 4) * 25, info.le.latency, info.le.timeout);

            /* Mirror upstream gap.c: do NOT drive a sub-spec interval from the
             * peripheral. Upstream requests itvl_min=6 (7.5 ms) and otherwise
             * keeps the interval/timeout NS2 chose; it only accepts a sub-spec
             * interval if NS2 proposes one (on_le_param_req). Pushing 5 ms at
             * the console mid-handshake is a divergence that can destabilize the
             * pairing connection.
             *
             * Only request the update when NS2's interval is at/above our 7.5 ms
             * floor. If NS2 already chose a sub-spec interval (< 6 units), leave
             * it untouched — requesting interval_max (= NS2's itvl) below
             * interval_min (= 6) is a malformed range, and pulling NS2 back up to
             * 7.5 ms would defeat the 5 ms goal. */
            if (interval >= 6) {
                struct bt_le_conn_param param = {
                    .interval_min = 6,          /* 7.5 ms (upstream itvl_min) */
                    .interval_max = interval,   /* keep NS2's negotiated itvl */
                    .latency = 0,
                    .timeout = info.le.timeout, /* keep NS2's supervision t/o */
                };
                int rc = bt_conn_le_param_update(conn, &param);
                if (rc != 0) {
                    ESP_LOGW(LOG_BLE_GAP, "conn param update request failed: %d", rc);
                }
            }
        } else {
            ESP_LOGE(LOG_BLE_GAP, "device not ready, resetting");
            device_status_set(DEV_BOOT);
        }
    }
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason) {
    ESP_LOGI(LOG_BLE_GAP, "disconnected, reason=%u, re-advertise in 3s", reason);

    if (default_conn) {
        bt_conn_unref(default_conn);
        default_conn = NULL;
    }

    /* Stop the HID report thread (mirrors upstream gap.c disconnect). */
    g_hid_controller.ops->stop_task(&g_hid_controller);

#if defined(CONFIG_BT_SMP) && CONFIG_JOY_BRIDGE_BLE_FAST_CONN_INTERVAL > 0
    k_work_cancel_delayable(&fast_interval_work);
#endif

    k_work_reschedule(&adv_restart_work, K_MSEC(3000));
}

static bool on_le_param_req(struct bt_conn *conn, struct bt_le_conn_param *param) {
    ARG_UNUSED(conn);
    /* Accept the console's proposed parameters verbatim — the Zephyr analog of
     * NimBLE's *self_params = *peer_params. With CONFIG_BT_CONN_PARAM_ANY the
     * host performs no further validation, so sub-spec intervals pass through
     * to the controller (which is the remaining gate; see file header). */
    ESP_LOGD(LOG_BLE_GAP, "le_param_req: itvl %u-%u latency %u timeout %u",
             param->interval_min, param->interval_max, param->latency, param->timeout);
    return true;
}

static void on_le_param_updated(struct bt_conn *conn, uint16_t interval,
                                uint16_t latency, uint16_t timeout) {
    ESP_LOGI(LOG_BLE_GAP, "conn params updated: interval=%u (%u.%02u ms) latency=%u timeout=%u",
             interval, (interval * 5) / 4, ((interval * 5) % 4) * 25, latency, timeout);
#if defined(CONFIG_BT_SMP) && CONFIG_JOY_BRIDGE_BLE_FAST_CONN_INTERVAL > 0
    /* NS2 connects at ~5 ms but unilaterally downgrades to a ~30 ms power-save
     * interval a few seconds in (observed on hardware), and nothing watches for
     * it — so the link is stuck slow and input lags. Whenever the interval
     * regresses above our target, re-arm the fast-interval request to pull it
     * back down. Gated on L4 so it can never fire during the pairing handshake
     * (pushing a sub-spec interval mid-handshake can destabilise pairing). */
    if (interval > CONFIG_JOY_BRIDGE_BLE_FAST_CONN_INTERVAL &&
        bt_conn_get_security(conn) >= BT_SECURITY_L4) {
        fast_interval_attempts = 0;
        k_work_reschedule(&fast_interval_work, K_MSEC(1000));
    }
#else
    ARG_UNUSED(conn);
#endif
}

#if defined(CONFIG_BT_SMP)
static void on_security_changed(struct bt_conn *conn, bt_security_t level,
                                enum bt_security_err err) {
    ARG_UNUSED(conn);
    ESP_LOGI(LOG_BLE_GAP, "security changed: level=%d err=%d", level, err);
#if CONFIG_JOY_BRIDGE_BLE_FAST_CONN_INTERVAL > 0
    if (err == BT_SECURITY_ERR_SUCCESS && level >= BT_SECURITY_L4) {
        /* Settle, then ask NS2 for the faster interval (post-pairing only),
         * retrying since the NS2 applies it only intermittently. */
        fast_interval_attempts = 0;
        k_work_reschedule(&fast_interval_work, K_MSEC(1500));
    }
#endif
}
#endif

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
    .le_param_req = on_le_param_req,
    .le_param_updated = on_le_param_updated,
#if defined(CONFIG_BT_SMP)
    .security_changed = on_security_changed,
#endif
};

/* Called by gatt.c on HID-input CCC change: start/stop the report thread. */
void ble_on_hid_subscription(bool enabled) {
    if (enabled) {
        /* Stop any running report thread before re-initialising the buffers, so
         * hid_reset never races a live reader, then (re)start it. */
        g_hid_controller.ops->stop_task(&g_hid_controller);
        g_hid_controller.ops->hid_reset(&g_hid_controller);
        g_hid_controller.ops->start_task(&g_hid_controller);
    } else {
        g_hid_controller.ops->stop_task(&g_hid_controller);
    }
}

/* ---- Identity ------------------------------------------------------------- */

static void ble_apply_identity(void) {
    /* Apply the controller's fixed Nintendo-prefixed PUBLIC address.
     *
     * bt_id_create() with a public address returns -ENOSYS on ESP32-S3, so we
     * instead set the controller's ESP_MAC_BT (the upstream
     * esp_iface_mac_addr_set path). The esp32 controller reads it via
     * esp_read_mac(ESP_MAC_BT) during init, and the Zephyr host then adopts it
     * as the default identity (BT_ID_DEFAULT). MUST be called BEFORE bt_enable().
     *
     * esp_iface_mac_addr_set expects big-endian MAC order == g_..._firmware.addr
     * (addr_re is the little-endian form, used only for display here). */
    esp_err_t err = esp_iface_mac_addr_set(g_controller_firmware.addr, ESP_MAC_BT);
    if (err != ESP_OK) {
        ESP_LOGW(LOG_APP, "esp_iface_mac_addr_set(ESP_MAC_BT) failed: %d", err);
        return;
    }
    ESP_LOGI(LOG_APP, "controller BT MAC set to:");
    log_print_addr(g_controller_firmware.addr_re);
    /* adv_id stays BT_ID_DEFAULT (the controller's public addr is now ours). */
}

/* ---- Stack init ----------------------------------------------------------- */

int ble_stack_init(void) {
    /* Set the controller public address BEFORE bt_enable so the controller
     * adopts it as its (and thus the host's default) identity. */
    ble_apply_identity();

    int rc = bt_enable(NULL);
    if (rc != 0) {
        ESP_LOGE(LOG_APP, "bt_enable failed: %d", rc);
        return rc;
    }
    ESP_LOGI(LOG_APP, "Bluetooth initialized");

    if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
        settings_load();
    }

    /* Leave the controller's min-conn-interval floor at the driver default,
     * exactly like upstream. The btdm driver already calls
     * ble_min_conn_interval_enable(3) (=3.75 ms) during controller init; we just
     * log the resulting floor. Upstream pairs with NS2 using this same floor=3,
     * which proves NS2's CONNECT_IND uses interval >= 3 — so forcing it lower is
     * unnecessary. More importantly, re-invoking ble_min_conn_interval_enable()
     * AFTER the controller is already running (as we used to, with arg 1) is a
     * divergence from upstream that can leave the LL's sub-spec connection-param
     * range inconsistent: a generic central at 30 ms (e.g. a Mac) still
     * connects, but NS2's sub-spec CONNECT_IND is dropped at the LL with no host
     * event — which is exactly the symptom we see. */
    ESP_LOGI(LOG_APP, "ctrl min conn interval floor = %u (driver default)",
             g_ble_con_interval_min);

    /* Resolve notify attributes + init the NS2 command system. */
    ble_gatt_init();

    /* If we already have a stored LTK, switch to wake/reconnect advertising and
     * inject the bond so the console can re-encrypt without re-pairing. */
    if (g_controller_firmware.paired) {
        g_adv_opcode = 0x81;
        if (inject_pairing_info_to_ble_ctx() != 0) {
            ESP_LOGE(LOG_APP, "Failed to inject pairing info");
        } else {
            ESP_LOGI(LOG_APP, "Pairing info injected");
        }
    }

    ble_advertise();
    return 0;
}
