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

# Always-built portable core + platform glue.
file(GLOB_RECURSE app_sources
  src/main.c
  src/app/*.c
  src/buffer/*.c
  src/controller/*.c
  src/codec/*.c
  src/protocol/*.c
  src/ble/*.c
  src/transport/*.c
  src/storage/*.c
)

target_sources(app PRIVATE ${app_sources})
