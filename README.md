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

The generated merged HEX file is `build\merged.hex`.

## Current firmware behavior

- `MODE` high: BLE HID keyboard mode.
- `MODE` low: USB HID keyboard mode.
- The hardware OFF position is expected to remove or gate system power. The firmware still has an internal `APP_MODE_OFF` state for future boards that expose OFF as a readable GPIO.
- Matrix scanning uses 6 rows x 4 columns with active-low row scanning and pulled-up columns.
- NumLock toggles the local numeric/navigation layer and is also sent to the host.
- EC11 rotation sends Consumer Control volume up/down; pressing the encoder switch sends mute.
- USB mode shows green RGB status and sends only USB HID reports.
- BLE mode shows blue RGB status, advertises as `AI_BLE_KEYBOARD`, stores bonds in NVS, and sends only BLE HID reports.
- The ST7789V status display uses a 320 x 172 active area with a 34-line Y offset. It shows mode, connection state, battery percentage/voltage, NumLock state, and date/time.

## Power behavior

- Battery percentage is calibrated as 4.2 V full and 3.3 V empty.
- The IP5306 keepalive pulse stays enabled while the firmware is in BLE or USB mode so the power module does not shut down under light load.
- The firmware does not use System OFF or deep sleep. After about 2 seconds without key activity, matrix scanning enters an idle state and arms column GPIO interrupts. Any key press wakes the main loop and restores the 5 ms active scan interval.
- BLE and USB links are kept alive during matrix idle. Switching MODE releases all pressed keys before changing the active HID transport to avoid stuck keys.

## Runtime logs

- Useful `LOG_INF` lines remain for startup, mode changes, BLE connection state, matrix idle, and key wakeup.
- Per-key scan logs and encoder bring-up candidates are `LOG_DBG` to keep RTT readable during normal use. Raise the log level or temporarily change those lines back to `LOG_INF` when debugging key mapping.

## Bring-up checklist

- BLE mode: pair once, reboot the keyboard, confirm it reconnects without repeated security failures.
- USB mode: confirm Windows shows an HID keyboard and keypad input works in a text editor.
- Idle wakeup: wait for `Keyboard idle: matrix wakeup armed`, press any key, and confirm `Keyboard wakeup: active scan`.
- MODE switch: confirm USB disconnects BLE intentionally and BLE starts advertising again when switched back.
- Power: run on battery for at least 30 minutes and confirm the IP5306 does not shut down under idle load.

## Hardware notes

The pin mapping is derived from `SCH_蓝牙小键盘_2026-03-28.pdf` and `E73-2G4M08S1C_UserManual_CN_v2.4.pdf`. If a bring-up test shows a swapped key row/column or an inverted MODE state, update `boards/me/ai_ble_keyboard/ai_ble_keyboard.dts`; application code reads all board pins from devicetree.
