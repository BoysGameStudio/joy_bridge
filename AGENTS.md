# AGENTS.md — joy_bridge

Zephyr RTOS port of the ESP-IDF/NimBLE project **ESP32-BLE5-NSController-Emulator**.
It emulates a Nintendo Switch **Pro2** controller to a **Switch 2 (NS2)** console over
BLE. Target board: `esp32s3_devkitc/esp32s3/procpu`.

**Status:** pairs with a real NS2 — the controller appears on Change Grip/Order,
security level 4, HID reports flow — and auto-reconnects to a paired NS2 after a
power-cycle (stored bond, level 4, no re-pair). The host-input path is
intentionally inert (see *Current state*).

Upstream reference project: `/Users/alan/Projects/ESP32-BLE5-NSController-Emulator`.

## Build / flash / test

All tooling lives in a conda env; export before any `west` / `twister`:

```sh
export PATH=/opt/homebrew/Caskroom/miniconda/base/envs/zephyr/bin:$PATH
export ZEPHYR_BASE=/Users/alan/Projects/ZephyrProjects/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/Users/alan/Zephyr/zephyr-sdk-1.0.1
# one-time: west blobs fetch hal_espressif
```

- **Build:** `west build -b esp32s3_devkitc/esp32s3/procpu .` → `build/` (the dev/flash
  dir; ~323 KB text, `zephyr.bin` ~428 KB).
- **Flash:** `west flash -d build --esp-device /dev/cu.usbserial-3120 --esp-baud-rate 460800`
  - The runner default **921600 FAILS** on this USB-serial bridge ("chip stopped
    responding" right after the baud switch, nothing written). Use **460800 or 115200**.
- **Test (CI gate):** `twister -T . -p esp32s3_devkitc/esp32s3/procpu -c -O twister-out --inline-logs`.
  The suite is `build_only` (it's firmware — nothing runs on host), so a clean build with
  `0 errored` is the pass. Twister builds with `-Werror`
  (`CONFIG_COMPILER_WARNINGS_AS_ERRORS=y`), so deprecations that a plain `west build` only
  *warns* about will FAIL CI — always verify under `-Werror` before declaring green.

## Source layout

```
include/   mirrors upstream main/include
src/
  app/        device state, utils, log shim
  buffer/     zero-copy SPSC ring buffer
  controller/ HID report engine (k_thread + double buffer)
  codec/      NS2 app protocol (ns2_codec / pro2 / ns2)
  protocol/   generic input-protocol router (no protocol wired)
  ble/        Zephyr bt host glue (ble / gatt / ble_store)
  transport/  UART backend + dispatcher
  storage/    settings-backed persistence
```

## Architecture invariants (keep these)

- **Logging:** ESP_LOGx → Zephyr LOG via `include/app_log.h` shim. Every `.c` that logs must
  `LOG_MODULE_REGISTER(jb_<name>, …)` *before* including `app_log.h`.
- **Portable core stays host-agnostic:** controller / codec / protocol / buffer never include
  `<zephyr/bluetooth/*>`; they go through `include/ble/ble_iface.h`. All NimBLE→Zephyr glue is
  confined to `src/ble/`.
- FreeRTOS → `k_thread` / `k_msleep`; NVS → `include/storage.h` over the settings subsys;
  AES-128-ECB → `bt_encrypt_be`.
- GATT is a static table: `BT_GATT_SERVICE_DEFINE` + `bt_gatt_find_by_uuid` caches `0x000e`
  (HID notify) and `0x001e` (cmd response).

## Load-bearing config — DO NOT regress

NS2 connects at 15 ms, identically to upstream. Pairing was solved by THREE things; earlier
5 ms-interval, controller-blob, and advertising theories were all **red herrings — don't
re-investigate them**. Each fix is documented inline in `prj.conf` / `src/ble/gatt.c`.

1. **Exact genuine-Pro2 GATT table (handles `0x0001`–`0x0032`).** In `src/ble/gatt.c`:
   services named `_0_svc_a` / `_0_svc_b` so `SORT_BY_NAME` places them *before* Zephyr's
   `_1_gatt_svc` / `_2_gap_svc`; per-characteristic vendor descriptors (`679d5510-…` on the
   R/N chars, `b746df8c-…` on the N chars) so value handles land exactly. `prj.conf`:
   `BT_GAP_SVC=n` + `BT_GATT_GAP_SVC_VALIDATE=n` (drop GAP `0x1800`; the validate check else
   makes `bt_enable` return `-EINVAL`) + `BT_GATT_SERVICE_CHANGED=n` (shrink GATT `0x1801` to
   a 1-handle stub). NS2 silently declines any non-Pro2 table.
2. **ATT MTU = 512.** `CONFIG_BT_BUF_ACL_RX_SIZE=516` (server RX MTU = min(ACL_RX−4,
   TX_MTU=512)). At 247, NS2 goes silent immediately after the MTU exchange.
3. **BT RX stack = 4096.** `CONFIG_BT_RX_STACK_SIZE=4096`. The cmd-`0x15` LTK exchange runs
   `aes128_ecb` on the BT RX workqueue; the default 1200 B overflowed → scheduler fault
   mid-pairing.

Also load-bearing:
- **Identity:** `bt_id_create(public)` returns `-ENOSYS` on S3. `ble_apply_identity()` calls
  `esp_iface_mac_addr_set(addr, ESP_MAC_BT)` (big-endian) *before* `bt_enable`; the controller
  adopts it as `BT_ID_DEFAULT` (→ `78:81:8C:96:E5:AD`).
- **SMP:** `BT_SMP_ENFORCE_MITM=n` + `BT_SMP_SC_PAIR_ONLY=n` (Just-Works / legacy /
  unauthenticated, to match NS2). `BT_GATT_CACHING=n`.

## 5 ms connection interval

NS2 drives a sub-spec ~5 ms (4-unit) interval. **Host:** solved by `CONFIG_BT_CONN_PARAM_ANY=y`
+ the accepting `le_param_req` in `ble.c`. **Controller:** the legacy btdm blob already enables
a 3.75 ms floor (`g_ble_con_interval_min=3`, verified at the binary level — the real impl is in
the blob's `arch_main.o`; the HAL no-op stub is *not* compiled for S3). So **no re-pin / un-stub
is needed**; only on-hardware confirmation of the negotiated interval remains. `ble.c` only
requests a param update when NS2's interval ≥ 6 units, avoiding a malformed `min>max` on the
sub-spec path.

## Current state & known gaps

- **Host input is inert by design.** The EasyCon protocol layer was removed; the generic router
  (`protocol.h` / `protocol_router.c`) is kept as the extension point but nothing is wired, so
  the UART transport drains bytes into the RX ring and nothing decodes them. To make it a usable
  input device, add a `protocol_instance_t` and assign `g_protocol_inst` in `transport.c`.
- `transport_uart.c` uses `DT_CHOSEN(zephyr_console)` — it shares the console UART. Add a
  `zephyr,joy-bridge-uart` chosen node in `app.overlay` before relying on host input + logs at
  the same time.
- Auto-reconnect to a paired NS2 is verified working (stored bond → security level 4 with no
  cmd-0x15 re-key). It requires `pro2_host_addr_init()` (pro2.c) to restore the saved NS2
  console address on boot; without it the wake-adv payload and the injected LTK bond
  (`ble_store.c`) both target `00:00:..:00` and the console can't reconnect.
- PSRAM is disabled in `prj.conf` (the octal-mode config from the hello-world overlay hung early
  SoC init); re-enable with the correct mode only if needed.

## Gotchas

- **Flash baud:** 921600 fails; use 460800/115200 (see *Build / flash / test*).
- **Flaky USB-serial auto-reset:** RTS/DTR toggling on `/dev/cu.usbserial-3120` can drop the
  board into download mode (`boot:0x0`). Recover with
  `esptool --port /dev/cu.usbserial-3120 --after hard-reset flash-id` (returns it to RUN +
  advertising).
- **Connectivity oracle:** `bleak` from the Mac (in the conda env) connects on demand and serves
  the full GATT table — use it to verify advertising/GATT without touching the flaky serial.
- **Benign boot lines:** `settings: set-value failure key bt/hash error(-2)` (no stored DB hash,
  caching off); `Num of Controller's ACL packets != ACL bt_conn_tx contexts (12 != 10)`.

## Protocol capture (DIAG hexdump)

`CONFIG_JOY_BRIDGE_DIAG_HEXDUMP=y` logs every NS2↔controller ATT exchange as `[DIAG]`-tagged INFO
hexdumps from `src/ble/gatt.c` (the on-wire boundary): RX cmd writes `0x0016`, TX cmd responses
`0x001e`, other inbound writes, constant reads, and CCC subscribes. Those raw bytes already carry
the cmd-`0x15` key material — **A1** (in an RX) and **B1** (in a TX) — so a serial capture alone is
enough to derive the link LTK offline (it's their XOR; exact byte order in `ns2_codec.c`
cmd-0x15/subcmd-0x04 and `ble_store.c` `ltk_re`). Add `CONFIG_JOY_BRIDGE_DIAG_HEXDUMP_HID=y` to also
dump HID input reports (`0x000e`, every 5–15 ms — verbose, can overrun the log backend). Needs the
JOY_BRIDGE log level ≥ INF. Macros live in `include/diag.h`; both compile to nothing when off
(verified clean under `-Werror`). grep serial for `[DIAG]`, then feed the bytes to Wireshark via
`text2pcap`. `scripts/capture_pair.py` does a one-shot capture that auto-stops on a real
cmd-`0x15` + level 4 (erase the bond first — see the script header — else you capture a silent
reconnect). Example traces: `docs/ns2_pair_capture.txt` (fresh pair, full `0x15` exchange) and
`docs/ns2_reconnect_capture.txt` (auto-reconnect, which skips `0x15`).

## If NS2 pairing regresses

The method that cracked it: symmetric greppable `[DIAG]` INFO logging added to BOTH firmwares
(joy_bridge `src/ble/{ble,gatt}.c` and the upstream `main/src/{gatt,gap,device}.c`, including
per-access ATT logging in the read/write callbacks), then back-to-back NS2 serial captures
@115200, with the upstream success trace as the reference. Confirmed-good trace: connect itvl=12
→ MTU tx=512 rx=512 → subscribe `0x001b/0x001f/0x0023` → cmd writes on `0x0016`
(`0x07/0x02/0x10/0x16`, then `0x15/01,04,02,03` with no crash) → security level 4 →
subscribe `0x000f` (HID `0x000e` CCC) → HID reports.
