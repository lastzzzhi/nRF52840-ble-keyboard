# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ai_ble_keyboard)

zephyr_compile_definitions(CONFIG_NFCT_PINS_AS_GPIOS)

target_include_directories(app PRIVATE src)

target_sources(app PRIVATE
  src/display.c
  src/hid_transport.c
  src/keymap.c
  src/key_wakeup_pm.c
  src/keyboard_matrix.c
  src/main.c
  src/power.c
  src/rgb.c
)
