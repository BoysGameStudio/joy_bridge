# SPDX-License-Identifier: Apache-2.0
#
# Standalone native_sim ztest for the pure boot-keyboard parser. Compiles ONLY
# src/input/kbd_parse.c (absolute path) with the repo's include/ dir — proving
# the parser has zero Zephyr dependencies.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(kbd_parse_test)

get_filename_component(JOY_BRIDGE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)

target_include_directories(app PRIVATE ${JOY_BRIDGE_ROOT}/include)

target_sources(app PRIVATE
  src/main.c
  ${JOY_BRIDGE_ROOT}/src/input/kbd_parse.c
)
