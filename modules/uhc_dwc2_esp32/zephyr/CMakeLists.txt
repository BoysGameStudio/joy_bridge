# SPDX-License-Identifier: Apache-2.0
#
# Native ESP32-S3 USB-OTG host controller (UHC) driver for the DesignWare DWC2
# core. Built when CONFIG_UHC_DWC2=y.
#
# The driver needs two things the default Zephyr build does not provide:
#   1. The private UHC driver header (uhc_common.h) under drivers/usb/uhc.
#   2. The Espressif host USB HAL (usb_dwc_hal.c) + USB PHY (usb_phy.c) sources,
#      which hal_espressif vendors but only compiles usb_wrap_hal.c from (gated on
#      CONFIG_UHC_DWC2). The HAL's hal/soc/esp_private include dirs are exposed
#      globally by hal_espressif, so these compile here too.

if(CONFIG_UHC_DWC2)
  if(NOT ZEPHYR_HAL_ESPRESSIF_MODULE_DIR)
    message(FATAL_ERROR "CONFIG_UHC_DWC2=y but hal_espressif module dir not found")
  endif()

  zephyr_library()
  zephyr_library_sources(${ZEPHYR_CURRENT_MODULE_DIR}/src/uhc_dwc2_esp32.c)
  zephyr_library_include_directories(
    ${ZEPHYR_CURRENT_MODULE_DIR}/include
    ${ZEPHYR_BASE}/drivers/usb/uhc          # uhc_common.h (private UHC header)
  )

  zephyr_library_sources(
    ${ZEPHYR_HAL_ESPRESSIF_MODULE_DIR}/components/esp_hal_usb/usb_dwc_hal.c
    ${ZEPHYR_HAL_ESPRESSIF_MODULE_DIR}/components/esp_hal_usb/usb_serial_jtag_hal.c
    ${ZEPHYR_HAL_ESPRESSIF_MODULE_DIR}/components/esp_hal_usb/${CONFIG_SOC_SERIES}/usb_dwc_periph.c
    ${ZEPHYR_HAL_ESPRESSIF_MODULE_DIR}/components/esp_hw_support/usb_phy/usb_phy.c
  )
endif()
