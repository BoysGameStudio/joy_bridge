/*
 * Bonding / LTK injection.
 *
 * Upstream (controller.c + device.c) injects a pre-derived LTK + peer address
 * straight into NimBLE's bonding store (ble_store_write_our_sec/_peer_sec) so an
 * already-paired NS2 console re-establishes encryption WITHOUT a fresh SMP
 * exchange. NS2 uses a fixed LTK with rand=0 / ediv=0 (key derived in the NS2
 * codec, cmd 0x15).
 *
 * Zephyr equivalent: insert a bond record via the host keys store
 * (subsys/bluetooth/host/keys.h, on the include path). We populate the SC and
 * legacy LTK fields with the (little-endian) LTK and zero rand/ediv so the
 * peripheral serves it on the controller's LE LTK request regardless of which
 * lookup SMP performs, then persist with bt_keys_store().
 *
 * NOTE (needs on-hardware validation): the exact key *type* NS2 expects is not
 * certain; we set LTK + LTK_P256 (+ PERIPH_LTK) to cover SC and legacy. If NS2
 * rejects encryption, narrow this to the type its handshake actually uses.
 */

#include <string.h>
#include <errno.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/addr.h>
#include "host/keys.h"

#include "device.h"
#include "controller/controller.h"
#include "utils.h"
#include "ble/ble_iface.h"
#include "ble_internal.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(jb_ble_store, CONFIG_JOY_BRIDGE_LOG_LEVEL);
#include "app_log.h"

int inject_pairing_info_to_ble_ctx(void) {
    bt_addr_le_t peer;
    peer.type = BT_ADDR_LE_PUBLIC;
    memcpy(peer.a.val, g_console_ns2.ble_addr.val, BT_ADDR_SIZE);

    struct bt_keys *keys = bt_keys_get_addr(ble_identity(), &peer);
    if (keys == NULL) {
        ESP_LOGE(LOG_APP, "bt_keys_get_addr failed (no free bond slot)");
        return -ENOMEM;
    }

    /* SC / legacy-central LTK (shared bt_ltk field), rand/ediv = 0. */
    memset(&keys->ltk, 0, sizeof(keys->ltk));
    memcpy(keys->ltk.val, g_controller_firmware.ltk_re, LTK_KEY_SIZE);
    keys->keys |= BT_KEYS_LTK | BT_KEYS_LTK_P256;

#if !defined(CONFIG_BT_SMP_SC_PAIR_ONLY)
    /* Legacy peripheral-distributed LTK. */
    memset(&keys->periph_ltk, 0, sizeof(keys->periph_ltk));
    memcpy(keys->periph_ltk.val, g_controller_firmware.ltk_re, LTK_KEY_SIZE);
    keys->keys |= BT_KEYS_PERIPH_LTK;
#endif

    keys->enc_size = LTK_KEY_SIZE;
    keys->flags |= BT_KEYS_AUTHENTICATED | BT_KEYS_SC;

    int rc = bt_keys_store(keys);
    if (rc != 0) {
        ESP_LOGE(LOG_APP, "bt_keys_store failed: %d", rc);
        return rc;
    }

    ESP_LOGI(LOG_APP, "Injected LTK bond (id=%u) for peer:", ble_identity());
    log_print_addr(g_console_ns2.ble_addr.val);
    log_print_ltk_hex("LTK", g_controller_firmware.ltk);
    return 0;
}
