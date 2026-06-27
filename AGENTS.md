# AGENTS.md — joy_bridge

Zephyr RTOS port of the ESP-IDF/NimBLE project **ESP32-BLE5-NSController-Emulator**.
It emulates a Nintendo Switch **Pro2** controller to a **Switch 2 (NS2)** console over
BLE. Target board: `esp32s3_devkitc/esp32s3/procpu`.

**Status:** pairs with a real NS2 — the controller appears on Change Grip/Order,
security level 4, HID reports flow — and auto-reconnects to a paired NS2 after a
power-cycle (stored bond, level 4, no re-pair). Host input is a wired **XInput
(Xbox) controller over USB host**, mapped onto the Pro2 — verified working
end-to-end on hardware (see *USB host / XInput controller input*). Holds NS2's
5 ms / 200 Hz interval (re-requested after NS2's power-save downgrade) and
deep-sleeps when the NS2 sleeps (BOOT-button wake) — both verified on hardware.

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
  app/        device state, utils, log shim, persistent event trace (trace.c)
  controller/ HID report engine (k_thread + double buffer)
  codec/      NS2 app protocol (ns2_codec / pro2 / ns2)
  ble/        Zephyr bt host glue (ble / gatt / ble_store)
  storage/    settings-backed persistence
  usb_host/   native ESP32-S3 UHC (DWC2) driver + USBH XInput class driver
  input/      XInput report -> Pro2 HID mapping
```

## Architecture invariants (keep these)

- **Logging:** ESP_LOGx → Zephyr LOG via `include/app_log.h` shim. Every `.c` that logs must
  `LOG_MODULE_REGISTER(jb_<name>, …)` *before* including `app_log.h`.
- **Portable core stays host-agnostic:** controller / codec never include
  `<zephyr/bluetooth/*>`; they go through `include/ble/ble_iface.h`. All NimBLE→Zephyr glue is
  confined to `src/ble/`.
- FreeRTOS → `k_thread` / `k_msleep`; NVS → `include/storage.h` over the settings subsys;
  AES-128-ECB → `bt_encrypt_be`.
- GATT is a static table: `BT_GATT_SERVICE_DEFINE` + `bt_gatt_find_by_uuid` caches `0x000e`
  (HID notify) and `0x001e` (cmd response).

## Load-bearing config — DO NOT regress

NS2 pairs at 15 ms, identically to upstream (the post-pairing 5 ms steady state is a separate
concern — see *5 ms connection interval*). Pairing was solved by THREE things; earlier 5 ms-interval,
controller-blob, and advertising theories were all **red herrings — don't re-investigate them**.
Each fix is documented inline in `prj.conf` / `src/ble/gatt.c`.

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

**Post-connect downgrade (verified on hardware):** NS2 reconnects at 5 ms, then power-saves to
~30 ms ~5 s in and holds it — so input lags once connected. Fix:
`CONFIG_JOY_BRIDGE_BLE_FAST_CONN_INTERVAL=4` (5 ms) + `on_le_param_updated()` re-requests it
whenever the interval regresses above target (gated on L4 so it can't perturb pairing). NS2 then
accepts and holds 5 ms; one re-request per session sufficed. `fast_interval_work_fn()` self-stops
at/under target, so the re-arm is what catches the *later* downgrade.

## NS2 sleep → deep sleep

`CONFIG_JOY_BRIDGE_SLEEP_ON_NS2_OFF=y` (selects `POWEROFF`). On any BLE disconnect — NS2
sleeping/powering off (HCI reason 0x13) or the link lost — `on_disconnected()` deep-sleeps the chip
(`sys_poweroff()` → `esp_deep_sleep_start()`, ~10 µA) instead of re-advertising; re-advertising the
wake packet would just wake the console back up. Wake source is the **BOOT button (GPIO0)** via ext1
(`esp_sleep_enable_ext1_wakeup`, `ANY_LOW` — `ALL_LOW` is deprecated on S3 and fails `-Werror`); a
tap reboots → re-advertise → reconnect. `log_panic()` flushes the deferred log before power-down
(this build routes `printk` through the log backend, so logs drop in bursts otherwise). Verified on
hardware: NS2 sleep → ~27 s of deep-sleep silence (console stayed asleep) → BOOT → reconnect.

## USB host / XInput controller input

The bridge's input source: a wired **XInput (Xbox 360 protocol)** controller plugged into the
ESP32-S3 USB-OTG port drives the emulated Pro2. Built when `CONFIG_JOY_BRIDGE_USB_XINPUT=y`
(default in `prj.conf`). XInput delivers structured reports straight into the HID engine, so
there is no separate byte-stream input transport.

**Why it's a from-scratch driver:** upstream Zephyr has **no DWC2 host (UHC) driver** for the S3
OTG core (`drivers/usb/uhc/` ships only max3421e / NXP mcux / virtual). XInput is also
vendor-specific (interface class `0xFF`, subclass `0x5D`, protocol `0x01`), not USB-HID, so it
needs a custom host class driver regardless. Both are written in-app:

```
src/usb_host/uhc_dwc2_esp32.{c,h}  native UHC host-controller driver over the Espressif USB HAL
src/usb_host/xinput_class.c        USBH class driver (matches 0xFF/0x5D/0x01, polls interrupt-IN)
src/usb_host/usb_host.c            usbh_init/enable; USBH_CONTROLLER_DEFINE bound to &usb_otg
src/input/xinput_map.c             XInput 20-byte report -> Pro2 HID back buffer (+ hid_commit)
include/input/xinput.h, include/usb_host.h
```

Data path: pad → `uhc_dwc2_esp32` (UHC) → Zephyr USBH stack → `xinput_class` (claims the gamepad
interface, submits an interrupt-IN poll) → `xinput_map_report()` → `g_hid_controller` back buffer
→ existing HID report thread → NS2. `main.c` calls `xinput_host_start()` **after** BLE init so a
USB failure can't block the load-bearing BLE path. The UHC driver is single-active (one channel
runs one `uhc_transfer` at a time, the model the UHC layer is built around — cf. `uhc_max3421e`).

**Build wiring (load-bearing — the host HAL is NOT compiled by default):**
- `CONFIG_UHC_DWC2=y` (app symbol in `Kconfig.dwc2`) makes hal_espressif compile `usb_wrap_hal.c`
  — its `zephyr/esp32s3/CMakeLists.txt:~295` already gates on this exact symbol.
- `CMakeLists.txt` additionally pulls the host HAL sources the module still doesn't compile:
  `esp_hal_usb/{usb_dwc_hal.c, usb_serial_jtag_hal.c, ${SOC}/usb_dwc_periph.c}` +
  `esp_hw_support/usb_phy/usb_phy.c` (via `${ZEPHYR_HAL_ESPRESSIF_MODULE_DIR}`), and adds private
  include dirs `${ZEPHYR_BASE}/drivers/usb/uhc` + `${ZEPHYR_BASE}/subsys/usb/host` (uhc_common.h /
  usbh_device.h / usbh_class.h — the same private headers the in-tree UVC host class uses).
- `boards/esp32s3_devkitc_procpu.overlay` enables `&usb_otg` (its `espressif,esp32-usb-otg` binding includes `snps,dwc2.yaml`
  → reg/interrupts/clocks; UDC device mode stays off so nothing else binds it). PHY is brought up
  in host mode via `usb_new_phy({CTRL_OTG, TARGET_INT, OTG_MODE_HOST, SPEED_FULL})`.

**Status: VERIFIED working end-to-end on hardware** (ESP32-S3-DevKitC-1 + a wired XInput pad,
2026-06-26): the pad enumerates, the class driver binds it, interrupt-IN reports flow with valid
data, and pressing buttons / moving sticks drives the NS2's on-screen controller — all while BLE
stays paired at security level 4. Builds clean under `-Werror` (twister green).

### Load-bearing transfer-engine fixes (hard-won on hardware — DO NOT regress)
Each was found by capturing the failing flow over serial and cross-checking the ESP-IDF HCD
(vendored in the upstream at `…/managed_components/espressif__usb/src/hcd_dwc.c`):
1. **250 ms connect debounce** (`UHC_DWC2_DEBOUNCE_MS`). At 25 ms the device wasn't stably attached,
   the port never enabled after reset (`port_en` stayed 0) and every transfer timed out. ESP-IDF
   uses `CONFIG_USB_HOST_DEBOUNCE_DELAY_MS=250`. **This was THE blocker for enumeration.**
2. **30 ms host-mode init delay** (`UHC_DWC2_INIT_DELAY_MS`) after `usb_dwc_hal_init` — the HAL
   contract ("≥25 ms before host mode takes effect"; ESP-IDF `INIT_DELAY_MS=30`).
3. **Synchronous bus reset** — `uhc_dwc2_bus_reset()` polls `usb_dwc_hal_decode_intr()` for the
   port-enable and configures the channel inline, rather than relying on the async ENABLED interrupt
   (which the worker, blocked on the driver lock during the reset, can miss).
4. **Periodic frame-list scheduling** for the interrupt endpoint: `usb_dwc_hal_port_set_frame_list`
   + `usb_dwc_hal_port_periodic_enable` after port-enable, and `ep_char.periodic.interval`; the
   periodic descriptor-list length is `FRAME_LIST_LEN` (32) vs 3 for control. Without this the INTR
   channel is never serviced.
5. **Reconfigure the channel only on target change** (`cur_ep`/`cur_addr`/`cur_mps`). Re-running
   `set_ep_char` / resetting the DATA toggle on every interrupt re-poll dropped the endpoint from the
   periodic schedule and corrupted DATA0/DATA1 — the pad delivered exactly one report then stalled.
6. **Silent control-transfer retry** (`UHC_DWC2_XFER_MAX_RETRIES`, `uhc_dwc2_fail_or_retry()`).
   Enumeration intermittently failed — `usbh_dev: Failed to read configuration 1 descriptor` on ~1-in-5
   boots — from a transient excessive-transaction-error glitch (marginal VBUS) on the config-descriptor
   control read; the host stack then frees the device and does NOT retry, so the pad never binds that
   boot. On a non-STALL **control** error the driver now restarts the whole transfer from SETUP (resets
   `xfer->stage` + `net_buf_reset` of any partial IN data), after a 5 ms settle, up to 3×; STALL
   (`-EPIPE`) and the interrupt-IN poll path are never retried (leaves the report path untouched).
   Re-activating the error-halted channel in place works (ESP-IDF's HCD does the same) — no hang.
   **Measured on hardware: ~80% → 10/10 enumeration; the retry fires on ~half of boots and recovers on
   the FIRST attempt** (it's a single transient transaction error, not sustained bad VBUS). DO NOT
   regress. This masks the symptom; the *root* cause is still the VBUS delicacy below.

### Reproduce / verify on hardware
1. Power the pad onto the native USB-C (OTG) VBUS externally (see VBUS below) and attach it.
2. Flash @460800; confirm NS2 pairing still reaches level 4 (BLE is the load-bearing invariant).
3. Serial shows `XInput controller bound (iface…ep…mps…)`; press buttons → the NS2 responds.

### Known characteristics / follow-ups
- **Idle report rate is modest** (~tens/s): the engine is single-active and re-arms each interrupt
  transfer through a full completion → class-cb → re-enqueue round trip (one-shot QTD + HOC halt per
  report). Button/stick *changes* register promptly (XInput reports on change). For a high steady
  rate, move to a continuous multi-buffer periodic poll (the ESP-IDF HCD scheme) — clean follow-up.
- **VBUS (hardware):** the internal PHY host mode does NOT source VBUS, and the DevKitC native USB-C
  VBUS is Schottky-isolated from 5 V — so the pad gets no power and never enumerates (LEDs dark). No
  firmware fix on a stock DevKitC: power the pad externally / direct-attach it (power-injecting OTG
  cable, or wire 5 V to the OTG VBUS; D+=GPIO20/D-=GPIO19). **Zephyr USBH has no hub support** — the
  pad must be on the root port, so a powered hub will NOT work. If a GPIO load switch is added, set
  `usb_phy` `otg_io_conf.drvvbus_io_num` to drive it.
- **IN transfer-size MPS rounding:** `uhc_dwc2_begin_stage()` passes `net_buf_tailroom` as the IN
  length verbatim (not rounded to MPS) — fine for the 8/64-byte control + 32-byte interrupt reads
  here; round up if a future endpoint misbehaves.
- **XInput mapping cross-validated against the proven upstream** (`xinput_host.c`): 0xFF/0x5D/0x01
  match, 20-byte report (type 0x00 / len 0x14), stick scale `(v+32768)>>4`, physical-position face
  map, triggers→digital ZL/ZR, **no stick-axis inversion** (`XINPUT_INVERT_L/RY` default 0). No
  controller init/LED command needed. Tunables in `xinput_map.c`: trigger threshold, optional stick
  deadzone, physical-vs-label button map.
- **Scope:** Xbox 360 XInput protocol only (20-byte report); Xbox One/Series (GIP) is out of scope.

## Current state & known gaps

- **Host input comes from USB host / XInput** (see that section): the pad's structured reports
  feed straight into the HID engine. There is no byte-stream input transport — the previously
  inert UART `transport` / `protocol_router` / ring-buffer scaffold has been removed. If a
  byte-stream host protocol is ever needed, add a parser that writes into `g_hid_controller`'s
  back buffer the way `src/input/xinput_map.c` does.
- Auto-reconnect to a paired NS2 is verified working (stored bond → security level 4 with no
  cmd-0x15 re-key). It requires `pro2_host_addr_init()` (pro2.c) to restore the saved NS2
  console address on boot; without it the wake-adv payload and the injected LTK bond
  (`ble_store.c`) both target `00:00:..:00` and the console can't reconnect.
- PSRAM is disabled in `prj.conf` (the octal-mode config from the hello-world overlay hung early
  SoC init); re-enable with the correct mode only if needed.

## Gotchas

- **Flash baud:** 921600 fails; use 460800/115200 (see *Build / flash / test*).
- **Board overlay naming:** the devicetree overlay is `boards/esp32s3_devkitc_procpu.overlay`.
  The `_procpu` qualifier suffix is REQUIRED for Zephyr to auto-discover it on this HWMv2 board —
  a bare `esp32s3_devkitc.overlay` (or a top-level `app.overlay`) is silently ignored and the
  build falls back to board defaults (4 MB flash map, USB-OTG disabled). Confirm it took via the
  `/* in …overlay */` provenance comments in `build/zephyr/zephyr.dts`.
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

## Persistent event trace (offline diagnostics)

`CONFIG_JOY_BRIDGE_PERSIST_LOG=y` (on by default in `prj.conf`) records a curated, low-rate trace
of significant runtime events to flash so the device's behaviour while running **offline** — on
battery, with no serial console attached — can be reviewed after the fact. Captured (via `jb_trace()`
calls co-located with the existing `LOG_*` lines, so live serial is unchanged): boot + reset cause,
BLE connect (NS2 addr + interval), disconnect + HCI reason, security level, conn-interval updates,
deep-sleep entry, and USB XInput bind/removal. `src/app/trace.c` + `include/trace.h`; compiles to
nothing when the symbol is off (verified clean under `-Werror`).

- **Never write flash inline — DO NOT REGRESS (hard-won on hardware).** Writing the on-chip SPI
  flash stalls the XIP instruction cache for the duration of the write. During the **boot log-burst**
  and the **NS2 pairing handshake** that corrupts the log backend (live console goes permanently
  silent) and **breaks pairing (security never reaches L4)** — verified by isolating the writes on
  hardware. A write once the link is *settled* (post-L4, HID flowing) is tolerated. So `jb_trace()`
  only formats the line and pushes it to a small RAM ring (`PEND_SLOTS`); it does **no flash I/O** and
  is safe from any context, including the BLE/USB callbacks during pairing. A low-priority worker
  (`flush_thread_fn`, `K_LOWEST_APPLICATION_THREAD_PRIO`) drains the ring to flash every ~2 s **only
  while `ble_hid_ready()`** (settled link), a few lines per cycle. `jb_trace_flush()` drains the ring
  **synchronously before deep sleep** (link already down → safe), called in `ns2_sleep_work_fn` before
  `log_panic()`. Cost: an abrupt power loss (battery yank) mid-session loses at most the last ~2 s of
  unflushed events; a clean session end (NS2-off deep sleep, or disconnect) flushes everything.
- **Backed by FCB on its OWN partition (`log_partition`)** — Zephyr's power-fail-safe Flash Circular
  Buffer. It never touches the settings/NVS partition that holds the load-bearing NS2 bond. The
  worker's write is `fcb_append`→`flash_area_write`→`append_finish`, 4-byte aligned (S3
  `write-block-size=4`); the line is padded and FCB records the true length so read-back is clean. The
  ring auto-rotates (erase-oldest) when full.
- **Retrieval — replay on boot:** on the next boot `jb_trace_init()` (first call in `main()`) dumps
  the most recent `TRACE_DUMP_LAST_N` (100) entries straight to the console UART via `uart_poll_out`
  (not the deferred log backend, which would drop a multi-line burst), bracketed by
  `==== persisted trace ====`. The dump is bounded (`fcb_offset_last_n`) because the ring holds
  thousands of lines before it wraps and the whole thing goes out byte-by-byte at 115200. So: plug into the
  PC, open the serial monitor, tap EN/RESET (or BOOT) → the offline session prints. Reset cause in
  the boot marker decodes battery/offline events: `RESET_BROWNOUT 0x04` (battery sag), `RESET_POR
  0x08`, `RESET_LOW_POWER_WAKE 0x80` (NS2-off deep-sleep wake), `RESET_PIN 0x01` (EN/RESET).
- **Retrieval — esptool pull** (no reboot): `esptool read-flash 0xfc0000 0x40000 trace.bin` then
  `strings trace.bin`.
- **Partition layout (load-bearing-adjacent):** the board overlay trims `storage_partition`'s tail
  from `0x1000000`→`0xfc0000` and carves `log_partition` = top 256 KB (`0xfc0000`, `0x40000`). The
  storage **start stays `0x3b0000`** and NVS fills from the start, so the existing bond (low sectors
  of a 12 MB partition) is untouched by removing the unused tail. `log_partition` needs
  `compatible = "zephyr,mapped-partition"` like every sibling in this AMP layout (the parent
  `partitions` node is not `fixed-partitions`, so `PARTITION_ID()` resolves via the mapped path —
  without the compat the build fails with an undeclared `…_PARTITION_ID`).

## If NS2 pairing regresses

The method that cracked it: symmetric greppable `[DIAG]` INFO logging added to BOTH firmwares
(joy_bridge `src/ble/{ble,gatt}.c` and the upstream `main/src/{gatt,gap,device}.c`, including
per-access ATT logging in the read/write callbacks), then back-to-back NS2 serial captures
@115200, with the upstream success trace as the reference. Confirmed-good trace: connect itvl=12
→ MTU tx=512 rx=512 → subscribe `0x001b/0x001f/0x0023` → cmd writes on `0x0016`
(`0x07/0x02/0x10/0x16`, then `0x15/01,04,02,03` with no crash) → security level 4 →
subscribe `0x000f` (HID `0x000e` CCC) → HID reports.
