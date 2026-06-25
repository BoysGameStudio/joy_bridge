/*
 * Key/value persistence backed by the Zephyr settings subsystem. Replaces the
 * upstream ESP-IDF NVS usage. Keys are stored as "jb/<namespace>/<key>".
 *
 * NOTE (scaffold): erase_namespace deletes the specific keys this app manages
 * under a namespace; a generic subtree wipe would need a load-handler that
 * enumerates children. The set we use (ltk / host / ctrl addr) is covered.
 */

#include "storage.h"

#include <string.h>
#include <errno.h>
#include <stdio.h>

#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(jb_storage, CONFIG_JOY_BRIDGE_LOG_LEVEL);
#include "app_log.h"

#define STORAGE_KEY_PREFIX "jb"
#define STORAGE_KEY_MAX    64

static bool s_inited;

static void make_key(char *buf, size_t buf_len, const char *ns, const char *key) {
    snprintf(buf, buf_len, STORAGE_KEY_PREFIX "/%s/%s", ns, key);
}

int storage_init(void) {
    if (s_inited) {
        return 0;
    }
    int rc = settings_subsys_init();
    if (rc != 0) {
        ESP_LOGE("storage", "settings_subsys_init failed: %d", rc);
        return rc;
    }
    s_inited = true;
    return 0;
}

struct read_ctx {
    void  *out;
    size_t cap;
    ssize_t ret;   /* bytes read, or negative on error; -ENOENT if untouched */
};

static int direct_loader(const char *key, size_t len, settings_read_cb read_cb,
                         void *cb_arg, void *param) {
    struct read_ctx *ctx = param;

    /* We loaded an exact full key; only the exact node (key == "") matches. */
    if (key != NULL && key[0] != '\0') {
        return 0;
    }
    size_t n = MIN(len, ctx->cap);
    ctx->ret = read_cb(cb_arg, ctx->out, n);
    return 0;
}

int storage_get_blob(const char *ns, const char *key, void *out, size_t *len) {
    if (!ns || !key || !out || !len) {
        return -EINVAL;
    }
    char full[STORAGE_KEY_MAX];
    make_key(full, sizeof(full), ns, key);

    struct read_ctx ctx = { .out = out, .cap = *len, .ret = -ENOENT };
    int rc = settings_load_subtree_direct(full, direct_loader, &ctx);
    if (rc != 0) {
        return rc;
    }
    if (ctx.ret < 0) {
        return (int)ctx.ret; /* -ENOENT when the key does not exist */
    }
    *len = (size_t)ctx.ret;
    return 0;
}

int storage_set_blob(const char *ns, const char *key, const void *in, size_t len) {
    if (!ns || !key || !in) {
        return -EINVAL;
    }
    char full[STORAGE_KEY_MAX];
    make_key(full, sizeof(full), ns, key);
    return settings_save_one(full, in, len);
}

int storage_get_u8(const char *ns, const char *key, uint8_t *out) {
    size_t len = sizeof(*out);
    return storage_get_blob(ns, key, out, &len);
}

int storage_set_u8(const char *ns, const char *key, uint8_t val) {
    return storage_set_blob(ns, key, &val, sizeof(val));
}

int storage_erase_namespace(const char *ns) {
    if (!ns) {
        return -EINVAL;
    }
    static const char *const managed_keys[] = {
        STORAGE_KEY_LTK,
        STORAGE_KEY_HOST_ADDR,
        STORAGE_KEY_CTRL_ADDR,
        STORAGE_KEY_CONTROLLER_TYPE,
    };
    char full[STORAGE_KEY_MAX];
    int last = 0;
    for (size_t i = 0; i < ARRAY_SIZE(managed_keys); i++) {
        make_key(full, sizeof(full), ns, managed_keys[i]);
        int rc = settings_delete(full);
        if (rc != 0 && rc != -ENOENT) {
            last = rc;
        }
    }
    return last;
}
