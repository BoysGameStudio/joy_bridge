cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(report_pacer_test)

# Pure parser under test: compiled standalone to prove it has no Zephyr deps.
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/controller/report_pacer.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
target_sources(app PRIVATE src/main.c)
