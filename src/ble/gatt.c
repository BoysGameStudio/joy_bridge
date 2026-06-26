/*
 * GATT server — Zephyr port of upstream gatt.c.
 *
 * Mirrors the NS2 Pro2 controller's two primary services and their 128-bit
 * characteristics. The data path that matters for emulation is wired up:
 *   - 0x000e  HID input report  (READ/NOTIFY)   -> ble_hid_notify()
 *   - 0x0016  command write      (WRITE_NO_RSP)  -> cmd_process() -> notify 0x001e
 *   - 0x001e  command response    (NOTIFY)
 *
 * The per-characteristic vendor descriptors (679d5510-… on the R/N chars,
 * b746df8c-… on the N chars) are defined here too — they are load-bearing for
 * the exact Pro2 handle layout NS2 requires (see the table notes below).
 * The READ characteristics return the upstream constant payloads; the dynamic
 * reads (0x000a/0x0026/0x002e) return empty, which NS2 pairing never exercises.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include "device.h"
#include "codec/ns2_codec.h"
#include "ble/ble_iface.h"
#include "ble_internal.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(jb_ble_gatt, CONFIG_JOY_BRIDGE_LOG_LEVEL);
#include "app_log.h"
#include "diag.h"

/* ---- 128-bit UUIDs (bytes are little-endian, matching upstream) ----------- */

/* Service 0x0001-0x0007: 00c5af5d-1964-4e30-8f51-1956f96bd280 */
static const struct bt_uuid_128 uuid_svc_a =
    BT_UUID_INIT_128(0x80, 0xd2, 0x6b, 0xf9, 0x56, 0x19, 0x51, 0x8f,
                     0x30, 0x4e, 0x64, 0x19, 0x5d, 0xaf, 0xc5, 0x00);
static const struct bt_uuid_128 uuid_0003 =
    BT_UUID_INIT_128(0x81, 0xd2, 0x6b, 0xf9, 0x56, 0x19, 0x51, 0x8f,
                     0x30, 0x4e, 0x64, 0x19, 0x5d, 0xaf, 0xc5, 0x00);
static const struct bt_uuid_128 uuid_0005 =
    BT_UUID_INIT_128(0x82, 0xd2, 0x6b, 0xf9, 0x56, 0x19, 0x51, 0x8f,
                     0x30, 0x4e, 0x64, 0x19, 0x5d, 0xaf, 0xc5, 0x00);
static const struct bt_uuid_128 uuid_0007 =
    BT_UUID_INIT_128(0x83, 0xd2, 0x6b, 0xf9, 0x56, 0x19, 0x51, 0x8f,
                     0x30, 0x4e, 0x64, 0x19, 0x5d, 0xaf, 0xc5, 0x00);

/* Service 0x0008-0x0032: ab7de9be-89fe-49ad-828f-118f09df7fd0 */
static const struct bt_uuid_128 uuid_svc_b =
    BT_UUID_INIT_128(0xd0, 0x7f, 0xdf, 0x09, 0x8f, 0x11, 0x8f, 0x82,
                     0xad, 0x49, 0xfe, 0x89, 0xbe, 0xe9, 0x7d, 0xab);
static const struct bt_uuid_128 uuid_000a =
    BT_UUID_INIT_128(0xd2, 0x7f, 0xdf, 0x09, 0x8f, 0x11, 0x8f, 0x82,
                     0xad, 0x49, 0xfe, 0x89, 0xbe, 0xe9, 0x7d, 0xab);
static const struct bt_uuid_128 uuid_000e =
    BT_UUID_INIT_128(0xf8, 0xc0, 0xfc, 0x5f, 0x75, 0x32, 0x58, 0x82,
                     0x19, 0x46, 0x3e, 0xec, 0x6c, 0x86, 0x92, 0x74);
static const struct bt_uuid_128 uuid_0012 =
    BT_UUID_INIT_128(0x05, 0x2b, 0xf7, 0x31, 0x0c, 0x63, 0x39, 0xa9,
                     0x7d, 0x42, 0x58, 0x92, 0x51, 0x3f, 0x48, 0xcc);
static const struct bt_uuid_128 uuid_0014 =
    BT_UUID_INIT_128(0x05, 0xf0, 0xe5, 0x4f, 0xa5, 0x1e, 0x44, 0xaf,
                     0x6c, 0x4e, 0xb7, 0x8e, 0xc9, 0x4a, 0x9d, 0x64);
static const struct bt_uuid_128 uuid_0016 =
    BT_UUID_INIT_128(0x79, 0xb3, 0xe8, 0x09, 0x98, 0x6f, 0xaf, 0x8e,
                     0xb5, 0x40, 0x55, 0x69, 0x7e, 0xbc, 0xac, 0x3d);
static const struct bt_uuid_128 uuid_0018 =
    BT_UUID_INIT_128(0x8d, 0x9f, 0xf5, 0x5d, 0x3e, 0xd2, 0xf7, 0xa4,
                     0xf7, 0x4d, 0xae, 0xfd, 0x3d, 0x42, 0x47, 0x41);
static const struct bt_uuid_128 uuid_001a =
    BT_UUID_INIT_128(0x6a, 0x83, 0x11, 0xb1, 0x15, 0x53, 0x0a, 0xa2,
                     0x36, 0x4d, 0xd8, 0xd9, 0x61, 0xa9, 0x65, 0xc7);
static const struct bt_uuid_128 uuid_001e =
    BT_UUID_INIT_128(0xe0, 0x57, 0x76, 0xa7, 0x6b, 0x32, 0x49, 0xa5,
                     0x95, 0x4e, 0x78, 0x42, 0x7d, 0x9f, 0x6d, 0x50);
static const struct bt_uuid_128 uuid_0022 =
    BT_UUID_INIT_128(0x80, 0x2a, 0x6d, 0x40, 0x6f, 0xf8, 0x15, 0xab,
                     0x41, 0x42, 0x1c, 0x84, 0xd2, 0x69, 0xbd, 0xd3);
static const struct bt_uuid_128 uuid_0026 =
    BT_UUID_INIT_128(0xde, 0x7f, 0xdf, 0x09, 0x8f, 0x11, 0x8f, 0x82,
                     0xad, 0x49, 0xfe, 0x89, 0xbe, 0xe9, 0x7d, 0xab);
static const struct bt_uuid_128 uuid_002a =
    BT_UUID_INIT_128(0xdf, 0x7f, 0xdf, 0x09, 0x8f, 0x11, 0x8f, 0x82,
                     0xad, 0x49, 0xfe, 0x89, 0xbe, 0xe9, 0x7d, 0xab);
static const struct bt_uuid_128 uuid_002c =
    BT_UUID_INIT_128(0x06, 0x2b, 0xf7, 0x31, 0x0c, 0x63, 0x39, 0xa9,
                     0x7d, 0x42, 0x58, 0x92, 0x51, 0x3f, 0x48, 0xcc);
static const struct bt_uuid_128 uuid_002e =
    BT_UUID_INIT_128(0xf9, 0xc0, 0xfc, 0x5f, 0x75, 0x32, 0x58, 0x82,
                     0x19, 0x46, 0x3e, 0xec, 0x6c, 0x86, 0x92, 0x74);
static const struct bt_uuid_128 uuid_0032 =
    BT_UUID_INIT_128(0x80, 0xb3, 0xe8, 0x09, 0x98, 0x6f, 0xaf, 0x8e,
                     0xb5, 0x40, 0x55, 0x69, 0x7e, 0xbc, 0xac, 0x3d);

/* Per-characteristic vendor descriptors present on the genuine Pro2. They carry
 * no data we need (upstream's gatt_dsc_access just returns empty), but they MUST
 * exist so the svc_b value handles land exactly on 0x000e/0x0016/0x001e/...:
 * each R/N or N characteristic on the real Pro2 has CCC + one of these. */
static const struct bt_uuid_128 uuid_dsc_679d = /* 679d5510-5a24-4dee-9557-95df80486ecb (R/N chars) */
    BT_UUID_INIT_128(0xcb, 0x6e, 0x48, 0x80, 0xdf, 0x95, 0x57, 0x95,
                     0xee, 0x4d, 0x24, 0x5a, 0x10, 0x55, 0x9d, 0x67);
static const struct bt_uuid_128 uuid_dsc_b746 = /* b746df8c-f358-495b-9cd2-e3bbeda4f979 (N chars) */
    BT_UUID_INIT_128(0x79, 0xf9, 0xa4, 0xed, 0xbb, 0xe3, 0xd2, 0x9c,
                     0x5b, 0x49, 0x58, 0xf3, 0x8c, 0xdf, 0x46, 0xb7);

/* ---- Static read payloads (upstream constants) ---------------------------- */

static uint8_t chr_0003_val[] = { 0x04, 0x00, 0x05, 0x00, 0x01, 0x01, 0x00 };
static uint8_t chr_0007_val[] = { 0x36, 0x80, 0x74, 0xee, 0xbb, 0x3d, 0x8e, 0x13 };

/* Cached value attributes for notifications (resolved in ble_gatt_init). */
static const struct bt_gatt_attr *hid_input_attr; /* 0x000e */
static const struct bt_gatt_attr *cmd_rsp_attr;   /* 0x001e */

/* HID input-report CCC subscription state. */
static volatile bool hid_notify_enabled;

/* ---- Access callbacks ------------------------------------------------------ */

static ssize_t read_const(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset) {
    const uint8_t *value = attr->user_data;
    size_t value_len = (attr->uuid == &uuid_0003.uuid) ? sizeof(chr_0003_val)
                     : (attr->uuid == &uuid_0007.uuid) ? sizeof(chr_0007_val)
                     : 0;
    LOG_DIAG("[DIAG] RD  h=0x%04x len=%u", attr->handle, (unsigned)value_len);
    LOG_DIAG_DUMP("RD", value, value_len);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, value_len);
}

static ssize_t read_empty(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(buf);
    ARG_UNUSED(len);
    ARG_UNUSED(offset);
    return 0; /* empty read, mirrors upstream gatt_svc_access default */
}

/* Vendor-descriptor sink: accept writes silently (mirrors upstream
 * gatt_dsc_access, which returns 0 for descriptor access). */
static ssize_t write_dsc(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(buf);
    ARG_UNUSED(offset);
    ARG_UNUSED(flags);
    return len;
}

static ssize_t write_noop(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(buf);
    ARG_UNUSED(offset);
    ARG_UNUSED(flags);
    LOG_DIAG("[DIAG] RX write h=0x%04x len=%u", attr->handle, (unsigned)len);
    LOG_DIAG_DUMP("RX write", buf, len);
    /* Accept and ignore (vibration / firmware-update / unknown sinks). */
    return len;
}

/* 0x0016: NS2 command write -> cmd_process() -> notify on 0x001e. */
static ssize_t write_cmd(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len < 8 + NS2_DATA_EMPTY_LEN) {
        ESP_LOGE(LOG_BLE_GATT, "Invalid cmd write length: %u", len);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    LOG_DIAG_DUMP("RX cmd 0x0016", buf, len);

    /* Upstream skips the NS2_DATA_EMPTY_LEN header; the command payload starts
     * there and is read-only, so point straight into the write buffer. */
    uint8_t *data_buf = (uint8_t *)buf + NS2_DATA_EMPTY_LEN;

    pro2_gatt_rsp_t rsp = { .cmd = data_buf[0], .subcmd = data_buf[3],
                            .rsp_data = NULL, .rsp_len = 0 };

    int rc = cmd_process(&rsp, data_buf, len);
    /* One concise INFO line per command so the cmd-0x15 pairing flow stays
     * visible without heavy per-packet logging. */
    ESP_LOGI(LOG_BLE_GATT, "cmd 0x%02x/0x%02x wlen=%u -> rc=%d rsp_len=%u",
             rsp.cmd, rsp.subcmd, len, rc, rsp.rsp_len);
    if (rc != 0) {
        ESP_LOGE(LOG_BLE_GATT, "cmd_process failed: %d", rc);
    } else if (cmd_rsp_attr && rsp.rsp_data && rsp.rsp_len) {
        LOG_DIAG_DUMP("TX rsp 0x001e", rsp.rsp_data, rsp.rsp_len);
        rc = bt_gatt_notify(conn, cmd_rsp_attr, rsp.rsp_data, rsp.rsp_len);
        if (rc != 0) {
            ESP_LOGW(LOG_BLE_GATT, "cmd response notify failed: %d", rc);
        }
    }

    if (rsp.rsp_data) {
        free(rsp.rsp_data);
    }
    /* WRITE_NO_RSP: the return value is not sent, but report bytes consumed. */
    return len;
}

static void ccc_hid_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr);
    hid_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    ESP_LOGI(LOG_BLE_GAP, "HID(0x000e) notify %s",
             hid_notify_enabled ? "enabled" : "disabled");
    ble_on_hid_subscription(hid_notify_enabled);
}

static void ccc_noop_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr);
    ARG_UNUSED(value);
    LOG_DIAG("[DIAG] CCC h=0x%04x val=0x%04x", attr->handle, value);
}

/* ---- Service A (0x0001-0x0007) -------------------------------------------- *
 * Named "_0_svc_a"/"_0_svc_b" on purpose: the static GATT services live in a
 * linker section sorted by name (SORT_BY_NAME), and Zephyr's own default
 * services are named "_1_gatt_svc" (0x1801) / "_2_gap_svc" (0x1800). A "_0_"
 * prefix sorts BEFORE them, so our vendor services take handles 0x0001+ — the
 * exact genuine-Pro2 layout NS2 expects. (GAP is already removed via
 * CONFIG_BT_GAP_SVC=n; the 1-handle GATT 0x1801 stub trails harmlessly after
 * svc_b at 0x0033.) */
BT_GATT_SERVICE_DEFINE(_0_svc_a,
    BT_GATT_PRIMARY_SERVICE(&uuid_svc_a),
    BT_GATT_CHARACTERISTIC(&uuid_0003.uuid, BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ, read_const, NULL, chr_0003_val),
    BT_GATT_CHARACTERISTIC(&uuid_0005.uuid, BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE, NULL, write_noop, NULL),
    BT_GATT_CHARACTERISTIC(&uuid_0007.uuid, BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ, read_const, NULL, chr_0007_val),
);

/* ---- Service B (0x0008-0x0032) -------------------------------------------- *
 * Each R/N or N characteristic carries CCC + the matching vendor descriptor so
 * every value handle lands exactly on the genuine-Pro2 number (see the running
 * handle comments). Removing any descriptor shifts everything below it. */
BT_GATT_SERVICE_DEFINE(_0_svc_b,
    BT_GATT_PRIMARY_SERVICE(&uuid_svc_b),                                    /* 0x0008 */

    /* 0x000a generic input report (R/N) */
    BT_GATT_CHARACTERISTIC(&uuid_000a.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ, read_empty, NULL, NULL),       /* 0x0009/0x000a */
    BT_GATT_CCC(ccc_noop_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),   /* 0x000b */
    BT_GATT_DESCRIPTOR(&uuid_dsc_679d.uuid, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                       read_empty, write_dsc, NULL),                         /* 0x000c */

    /* 0x000e Pro2 HID input report (R/N) — primary report path */
    BT_GATT_CHARACTERISTIC(&uuid_000e.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ, read_empty, NULL, NULL),       /* 0x000d/0x000e */
    BT_GATT_CCC(ccc_hid_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),    /* 0x000f */
    BT_GATT_DESCRIPTOR(&uuid_dsc_679d.uuid, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                       read_empty, write_dsc, NULL),                         /* 0x0010 */

    /* 0x0012 vibration (WNR) */
    BT_GATT_CHARACTERISTIC(&uuid_0012.uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, write_noop, NULL),      /* 0x0011/0x0012 */
    /* 0x0014 command (WNR) */
    BT_GATT_CHARACTERISTIC(&uuid_0014.uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, write_noop, NULL),      /* 0x0013/0x0014 */
    /* 0x0016 vibration + command (WNR) — NS2 command channel */
    BT_GATT_CHARACTERISTIC(&uuid_0016.uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, write_cmd, NULL),       /* 0x0015/0x0016 */
    /* 0x0018 firmware update (WNR) */
    BT_GATT_CHARACTERISTIC(&uuid_0018.uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, write_noop, NULL),      /* 0x0017/0x0018 */

    /* 0x001a notify */
    BT_GATT_CHARACTERISTIC(&uuid_001a.uuid, BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE, NULL, NULL, NULL),             /* 0x0019/0x001a */
    BT_GATT_CCC(ccc_noop_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),   /* 0x001b */
    BT_GATT_DESCRIPTOR(&uuid_dsc_b746.uuid, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                       read_empty, write_dsc, NULL),                         /* 0x001c */

    /* 0x001e command response notify */
    BT_GATT_CHARACTERISTIC(&uuid_001e.uuid, BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE, NULL, NULL, NULL),             /* 0x001d/0x001e */
    BT_GATT_CCC(ccc_noop_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),   /* 0x001f */
    BT_GATT_DESCRIPTOR(&uuid_dsc_b746.uuid, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                       read_empty, write_dsc, NULL),                         /* 0x0020 */

    /* 0x0022 notify */
    BT_GATT_CHARACTERISTIC(&uuid_0022.uuid, BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE, NULL, NULL, NULL),             /* 0x0021/0x0022 */
    BT_GATT_CCC(ccc_noop_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),   /* 0x0023 */
    BT_GATT_DESCRIPTOR(&uuid_dsc_b746.uuid, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                       read_empty, write_dsc, NULL),                         /* 0x0024 */

    /* 0x0026 read/notify */
    BT_GATT_CHARACTERISTIC(&uuid_0026.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ, read_empty, NULL, NULL),       /* 0x0025/0x0026 */
    BT_GATT_CCC(ccc_noop_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),   /* 0x0027 */
    BT_GATT_DESCRIPTOR(&uuid_dsc_679d.uuid, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                       read_empty, write_dsc, NULL),                         /* 0x0028 */

    /* 0x002a unknown (WNR) */
    BT_GATT_CHARACTERISTIC(&uuid_002a.uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, write_noop, NULL),      /* 0x0029/0x002a */
    /* 0x002c headset audio (WNR) */
    BT_GATT_CHARACTERISTIC(&uuid_002c.uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, write_noop, NULL),      /* 0x002b/0x002c */

    /* 0x002e read/notify */
    BT_GATT_CHARACTERISTIC(&uuid_002e.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ, read_empty, NULL, NULL),       /* 0x002d/0x002e */
    BT_GATT_CCC(ccc_noop_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),   /* 0x002f */
    BT_GATT_DESCRIPTOR(&uuid_dsc_679d.uuid, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                       read_empty, write_dsc, NULL),                         /* 0x0030 */

    /* 0x0032 unknown (WNR) */
    BT_GATT_CHARACTERISTIC(&uuid_0032.uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, write_noop, NULL),      /* 0x0031/0x0032 */
);

/* ---- Public glue ---------------------------------------------------------- */

void ble_gatt_init(void) {
    if (cmd_system_init() != 0) {
        ESP_LOGE(LOG_APP, "Failed to initialize command system");
    } else {
        ESP_LOGI(LOG_APP, "Command system initialized");
    }

    hid_input_attr = bt_gatt_find_by_uuid(_0_svc_b.attrs, _0_svc_b.attr_count, &uuid_000e.uuid);
    cmd_rsp_attr   = bt_gatt_find_by_uuid(_0_svc_b.attrs, _0_svc_b.attr_count, &uuid_001e.uuid);
    if (!hid_input_attr || !cmd_rsp_attr) {
        ESP_LOGE(LOG_BLE_GATT, "Failed to resolve notify attributes (0x000e/0x001e)");
    }
}

bool ble_hid_ready(void) {
    return ble_active_conn() != NULL && hid_notify_enabled && g_device_status == DEV_READY;
}

int ble_hid_notify(const uint8_t *data, size_t len) {
    struct bt_conn *conn = ble_active_conn();
    if (conn == NULL || hid_input_attr == NULL) {
        return -ENOTCONN;
    }
    LOG_DIAG_DUMP_HID("TX hid 0x000e", data, len);
    return bt_gatt_notify(conn, hid_input_attr, data, len);
}
