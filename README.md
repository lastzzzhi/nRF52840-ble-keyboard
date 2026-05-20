# AI BLE Keyboard

NCS 3.2.3 project for the E73-2G4M08S1C / nRF52840 numeric keypad board.

## Build in VS Code

1. Open this folder in VS Code.
2. Select the nRF Connect SDK installation at `D:\ncs\v3.2.3`.
3. Add a build configuration with board target `ai_ble_keyboard/nrf52840`.
4. Build and flash with the nRF Connect extension.

Validated command line build used for this workspace:

```powershell
$env:Path='D:\ncs\toolchains\fd21892d0f\opt\bin;D:\ncs\toolchains\fd21892d0f\mingw64\bin;' + $env:Path
$env:ZEPHYR_BASE='D:\ncs\v3.2.3\zephyr'
$env:ZEPHYR_TOOLCHAIN_VARIANT='zephyr'
$env:ZEPHYR_SDK_INSTALL_DIR='D:\ncs\toolchains\fd21892d0f\opt\zephyr-sdk'
$mods='D:/ncs/v3.2.3/nrf;D:/ncs/v3.2.3/modules/hal/nordic;D:/ncs/v3.2.3/modules/debug/segger;D:/ncs/v3.2.3/modules/hal/cmsis;D:/ncs/v3.2.3/modules/hal/cmsis_6;D:/ncs/v3.2.3/nrfxlib;D:/ncs/v3.2.3/modules/crypto/mbedtls;D:/ncs/v3.2.3/modules/crypto/oberon-psa-crypto;D:/ncs/v3.2.3/modules/lib/zcbor'
cmake -B build -S . -GNinja -DBOARD=ai_ble_keyboard/nrf52840 -DBOARD_ROOT=$PWD -DDTS_ROOT=$PWD "-DZEPHYR_MODULES:STRING=$mods" -DUSER_CACHE_DIR="$PWD\.zephyr_cache"
ninja -C build
```

The generated HEX file is `build\zephyr\zephyr.hex`.

## Current firmware behavior

- `MODE` high: BLE HID keyboard mode.
- `MODE` low: USB HID keyboard mode.
- The hardware OFF position is expected to remove or gate system power. The firmware still has an internal `APP_MODE_OFF` state for future boards that expose OFF as a readable GPIO.
- Matrix scanning uses 6 rows x 4 columns with active-low row scanning and pulled-up columns.
- NumLock toggles the local numeric/navigation layer and is also sent to the host.
- EC11 rotation sends Consumer Control volume up/down.
- WS2812 status color, battery percentage, mode, connection state, and NumLock state are shown on the SSD1306 display when the hardware is present.

## Hardware notes

The pin mapping is derived from `SCH_蓝牙小键盘_2026-03-28.pdf` and `E73-2G4M08S1C_UserManual_CN_v2.4.pdf`. If a bring-up test shows a swapped key row/column or an inverted MODE state, update `boards/me/ai_ble_keyboard/ai_ble_keyboard.dts`; application code reads all board pins from devicetree.
