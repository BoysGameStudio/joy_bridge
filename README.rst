joy_bridge — NS2 Pro2 emulator + XInput USB-host bridge (Zephyr)
###############################################################

Overview
********

``joy_bridge`` is a Zephyr RTOS port of the ESP-IDF project
**ESP32-BLE5-NSController-Emulator**. It emulates a Nintendo Switch **Pro2**
controller to a Nintendo Switch 2 (NS2) over BLE, and bridges a wired
**XInput (Xbox 360) controller** plugged into the ESP32-S3's USB-OTG port to the
NS2 — so a physical Xbox controller drives the Switch 2.

Target board: ``esp32s3_devkitc/esp32s3/procpu``.

Status
======

- **BLE Pro2 emulation:** pairs with a real NS2 (appears on Change Grip/Order,
  security level 4, HID reports flow) and auto-reconnects to a paired NS2 after a
  power-cycle (stored bond, no re-pair).
- **USB host / XInput input:** verified working end-to-end on hardware
  (ESP32-S3-DevKitC-1) — a wired XInput pad enumerates, binds, and drives the NS2
  while BLE stays paired at level 4.
- **Low latency:** holds NS2's 5 ms / 200 Hz connection interval. NS2 power-saves
  to ~30 ms a few seconds after connecting; the firmware re-requests 5 ms to pull
  it back (``CONFIG_JOY_BRIDGE_BLE_FAST_CONN_INTERVAL``).
- **NS2 sleep → deep sleep:** when the NS2 sleeps and the link drops, the bridge
  deep-sleeps (~10 µA) instead of re-advertising (which would wake the console
  back up). Tap BOOT to wake → reconnect (``CONFIG_JOY_BRIDGE_SLEEP_ON_NS2_OFF``).
- **Offline event journal:** significant events (boot + reset cause, BLE connect /
  disconnect, security level, conn-interval, deep sleep, USB bind) are mirrored to a
  power-fail-safe flash ring via the ``JOURNAL`` macro and replayed to the console on
  the next boot — so a headless / battery session can be reviewed after the fact
  (``CONFIG_JOURNAL``; the reusable ``modules/journal`` module).
- Builds clean under ``-Werror`` (twister ``build_only`` suite).

USB Host / XInput mode
======================

A wired XInput (Xbox 360 / compatible) controller plugged into the S3's native
USB-OTG port is read in USB host mode and mapped onto the emulated Pro2. Enabled
by ``CONFIG_JOY_BRIDGE_USB_XINPUT`` (default on for this build).

Upstream Zephyr has no DWC2 host (UHC) driver for the S3 OTG core, so this adds a
from-scratch one over the vendored Espressif USB HAL, packaged as the reusable
Zephyr module ``modules/uhc_dwc2_esp32`` (``CONFIG_UHC_DWC2``). On top of it sit a
USBH XInput class driver (``src/usb_host/xinput_class.c``, matches interface class
``0xFF`` / subclass ``0x5D`` / protocol ``0x01``) and the XInput→Pro2 mapping
(``src/input/xinput_map.c``). Face buttons map by physical position (bottom
button → Switch "B"); analog triggers map to digital ZL/ZR.

**Hardware note (VBUS).** The controller plugs into the S3's native USB port
(D+ = GPIO20, D- = GPIO19). That port must supply **5 V VBUS** to power the
controller, which the DevKitC does **not** do by default. Power the controller
externally (a power-injecting OTG cable, or wire 5 V to the OTG VBUS). If the
controller's LEDs never light up, VBUS is the first thing to check. Zephyr's USB
host stack has **no hub support** — the controller must attach directly to the
root port (a powered hub will not enumerate).

Scope: wired Xbox 360 / XInput-compatible controllers only; Xbox One/Series (GIP)
is out of scope.

5 ms connection interval
========================

NS2 drives a sub-spec ~5 ms interval. Both sides are handled:

- **Host:** ``CONFIG_BT_CONN_PARAM_ANY=y`` + the accepting ``le_param_req``
  callback.
- **Controller:** already enabled on this S3 build (verified at the binary
  level). The legacy btdm driver enables a 3.75 ms floor during controller init;
  the real implementation comes from the blob's ``arch_main.o`` and the HAL no-op
  stub is not compiled for S3 — so no re-pin or un-stub is needed.

After connecting, NS2 power-saves to ~30 ms; ``on_le_param_updated()`` re-requests
5 ms whenever the interval regresses, and NS2 then holds 5 ms (verified on
hardware). See the header of ``src/ble/ble.c`` for the full trace.

Source layout
=============

::

   include/            public headers (mirrors upstream main/include)
   src/
     app/        device state, utils, ESP->Zephyr logging shim
     controller/ HID report engine (pro2)
     codec/      NS2 codec (ns2_codec / pro2 / ns2)
     ble/        Zephyr bt host glue (ble / gatt / ble_store)
     storage/    settings-backed persistence
     input/      XInput -> Pro2 report mapping
     usb_host/   USBH XInput class driver + host glue (usbh_init)
     main.c      init sequence
   modules/            reusable in-tree Zephyr modules (ZEPHYR_EXTRA_MODULES)
     journal/         flash-backed offline event log (the JOURNAL macro)
     uhc_dwc2_esp32/  native ESP32-S3 DWC2 USB host-controller driver

Building and Running
********************

.. code-block:: console

   west build -b esp32s3_devkitc/esp32s3/procpu .
   west flash -d build --esp-baud-rate 460800

The Espressif BT controller blobs must be present
(``west blobs fetch hal_espressif``). Flash at **460800** baud — the 921600
default fails on the DevKitC USB-serial bridge.

For the full development guide — build/test commands, the conda toolchain env,
the load-bearing NS2-pairing fixes, and the USB host / XInput driver internals
and bring-up — see ``AGENTS.md``.
