# SPDX-License-Identifier: Apache-2.0

# Expose <journal/journal.h> unconditionally so includes resolve even when
# CONFIG_JOURNAL=n (the header self-stubs to no-ops in that case).
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include)

if(CONFIG_JOURNAL)
  zephyr_library()
  zephyr_library_sources(${ZEPHYR_CURRENT_MODULE_DIR}/src/journal.c)
endif()
