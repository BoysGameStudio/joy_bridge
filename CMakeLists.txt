# SPDX-License-Identifier: Apache-2.0
#
# joy_bridge — Zephyr port of ESP32-BLE5-NSController-Emulator
# (Nintendo Switch Pro2 controller emulation over BLE).
#
# Migrated from the upstream ESP-IDF project. The layering is preserved:
#   - portable core: app/ (utils, state) / controller(HID) / codec(ns2)
#   - platform glue (Zephyr): ble/ (bt host), storage/ (settings)
#   - host input: usb_host/ (XInput class + host glue) + input/ (XInput->Pro2);
#                 the native UHC driver is the modules/uhc_dwc2_esp32 module

cmake_minimum_required(VERSION 3.20.0)

# Reusable in-tree Zephyr modules (must be on ZEPHYR_EXTRA_MODULES before
# find_package(Zephyr) so their Kconfig + sources are picked up):
#   - journal:        persistent flash-backed offline event log (CONFIG_JOURNAL)
#   - uhc_dwc2_esp32: native ESP32-S3 DWC2 USB host controller (CONFIG_UHC_DWC2)
list(APPEND ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/modules/journal
  ${CMAKE_CURRENT_SOURCE_DIR}/modules/uhc_dwc2_esp32
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(joy_bridge C)

# Public headers (mirrors the upstream main/include tree).
target_include_directories(app PRIVATE include)

# Always-built portable core + platform glue. CONFIGURE_DEPENDS re-globs when
# files are added/removed so the build doesn't need a manual reconfigure.
file(GLOB_RECURSE app_sources CONFIGURE_DEPENDS
  src/main.c
  src/app/*.c
  src/controller/*.c
  src/codec/*.c
  src/ble/*.c
  src/storage/*.c
  src/input/*.c
  src/usb_host/*.c
)

target_sources(app PRIVATE ${app_sources})

# ---- USB host (input devices) -----------------------------------------------
# The native UHC host-controller driver now lives in the reusable module
# modules/uhc_dwc2_esp32 (added to ZEPHYR_EXTRA_MODULES above). The in-app class
# drivers (xinput_class.c / kbd_class.c) still use the experimental USB host
# stack's private class API (usbh_device.h / usbh_class.h) — no public header
# yet, same as the in-tree UVC host class — so they need that private include dir.
if(CONFIG_JOY_BRIDGE_USB_XINPUT OR CONFIG_JOY_BRIDGE_USB_KBD)
  target_include_directories(app PRIVATE ${ZEPHYR_BASE}/subsys/usb/host)
endif()
