# SPDX-License-Identifier: Apache-2.0
#
# joy_bridge — Zephyr port of ESP32-BLE5-NSController-Emulator
# (Nintendo Switch Pro2 controller emulation over BLE).
#
# Migrated from the upstream ESP-IDF project. The layering is preserved:
#   - portable core: buffer / utils / controller(HID) / codec(ns2) / protocol(router)
#   - platform glue (Zephyr): ble/ (bt host), transport/ (uart), storage/ (settings)

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(joy_bridge C)

# Public headers (mirrors the upstream main/include tree).
target_include_directories(app PRIVATE include)

# Always-built portable core + platform glue. CONFIGURE_DEPENDS re-globs when
# files are added/removed so the build doesn't need a manual reconfigure.
file(GLOB_RECURSE app_sources CONFIGURE_DEPENDS
  src/main.c
  src/app/*.c
  src/buffer/*.c
  src/controller/*.c
  src/codec/*.c
  src/protocol/*.c
  src/ble/*.c
  src/transport/*.c
  src/storage/*.c
  src/input/*.c
  src/usb_host/*.c
)

target_sources(app PRIVATE ${app_sources})

# ---- Native ESP32-S3 USB-OTG host controller (uhc_dwc2_esp32) ---------------
# The in-app UHC driver uses two things the Zephyr build does not provide by
# default:
#   1. The private UHC driver header (uhc_common.h) for the uhc_xfer_* /
#      uhc_submit_event helpers exported by drivers/usb/uhc/uhc_common.c.
#   2. The Espressif host USB HAL (usb_dwc_hal.c) and USB PHY (usb_phy.c)
#      implementations, which hal_espressif vendors but only compiles
#      usb_wrap_hal.c from (gated on CONFIG_UHC_DWC2). The HAL's soc/hal/
#      esp_private include dirs are exposed globally by the module, so these
#      compile against the app target.
if(CONFIG_UHC_DWC2)
  if(NOT ZEPHYR_HAL_ESPRESSIF_MODULE_DIR)
    message(FATAL_ERROR "CONFIG_UHC_DWC2=y but hal_espressif module dir not found")
  endif()
  # Private UHC + USB-host-stack headers (uhc_common.h; usbh_device.h /
  # usbh_class.h for the in-app XInput class driver — the experimental host
  # stack has no public class-driver API yet, same as the in-tree UVC class).
  target_include_directories(app PRIVATE
    ${ZEPHYR_BASE}/drivers/usb/uhc
    ${ZEPHYR_BASE}/subsys/usb/host
  )
  target_sources(app PRIVATE
    ${ZEPHYR_HAL_ESPRESSIF_MODULE_DIR}/components/esp_hal_usb/usb_dwc_hal.c
    ${ZEPHYR_HAL_ESPRESSIF_MODULE_DIR}/components/esp_hal_usb/usb_serial_jtag_hal.c
    ${ZEPHYR_HAL_ESPRESSIF_MODULE_DIR}/components/esp_hal_usb/${CONFIG_SOC_SERIES}/usb_dwc_periph.c
    ${ZEPHYR_HAL_ESPRESSIF_MODULE_DIR}/components/esp_hw_support/usb_phy/usb_phy.c
  )
endif()
