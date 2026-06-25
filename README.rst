joy_bridge — NS2 Pro2 Controller Emulator (Zephyr)
##################################################

Overview
********

``joy_bridge`` is a Zephyr RTOS port of the ESP-IDF project
**ESP32-BLE5-NSController-Emulator**. It emulates a Nintendo Switch Pro2
controller to a Nintendo Switch 2 (NS2) over BLE. A UART host-input transport
is scaffolded, but no application protocol is currently wired.

This is a **scaffold + portable-core migration**: the platform-agnostic logic
is fully ported, and the platform glue (BLE host, transport, storage) is laid
down as Zephyr skeletons with documented ``TODO``s where hardware-specific work
remains.

Status
======

- Builds for ``esp32s3_devkitc/esp32s3/procpu`` (~323 KB text).
- **Ported (functional):** zero-copy ring buffer, utils (AES via
  ``bt_encrypt_be``), controller/HID engine (Zephyr thread + double buffer),
  NS2 codec (command handlers + flash-sim).
- **Skeletons (compile, wired, need completion):**

  - ``src/ble/`` — Zephyr ``bt_*`` GATT service (mirrors the NS2 two-service
    table), advertising, connection callbacks, and the ``le_param_req`` accept
    path. ``inject_pairing_info_to_ble_ctx`` (LTK bond injection) is a
    documented no-op; needs the host keys store.
  - ``src/transport/`` — interrupt-driven UART backend.
  - ``src/storage/`` — settings-backed key/value (replaces NVS).

5 ms connection interval
========================

NS2 drives a sub-spec ~5 ms interval. Both sides are handled:

- **Host:** ``CONFIG_BT_CONN_PARAM_ANY=y`` + the accepting ``le_param_req``
  callback.
- **Controller:** already enabled on this S3 build (verified at the binary
  level). The legacy btdm driver calls ``ble_min_conn_interval_enable(3)``
  (3.75 ms floor) during controller init; the real implementation comes from
  the blob's ``arch_main.o`` and the HAL no-op stub is not compiled for S3 — so
  no re-pin or un-stub is needed. Only an on-hardware interval check remains.

See the header of ``src/ble/ble.c`` for the full trace.

Source layout
=============

::

   include/            public headers (mirrors upstream main/include)
   src/
     app/      device state, utils, logging shim
     buffer/   zero-copy ring buffer
     controller/  HID report engine (pro2)
     codec/    NS2 codec (ns2_codec / pro2 / ns2)
     protocol/ generic input-protocol router (no protocol wired)
     ble/      Zephyr bt host glue (ble / gatt / ble_store)
     transport/  UART backend + dispatcher
     storage/  settings-backed persistence
     main.c    init sequence

Building and Running
********************

.. code-block:: console

   west build -b esp32s3_devkitc/esp32s3/procpu .
   west flash

The Espressif BT controller blobs must be present
(``west blobs fetch hal_espressif``).
