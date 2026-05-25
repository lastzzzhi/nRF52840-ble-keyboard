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
- BLE advertising stays fast for 300 seconds after entering BLE mode, then falls back to slow advertising if no host is connected.
- The ST7789V status display uses a 320 x 172 active area with a 34-line Y offset. It shows mode, connection state, battery percentage/voltage, IP5306 charge state, NumLock state, and date/time.
- Date/time uses host-synchronized Unix time when the host tool sends `SYNC_TIME`; otherwise it falls back to the build timestamp plus device uptime.
- A 64-byte host-control protocol is available over USB HID Feature Report and over a custom BLE GATT service. See `docs/host_protocol.md`.

## Power behavior

- Battery percentage is calibrated as 4.2 V full and 3.3 V empty. IP5306 status is displayed as `BAT`, `CHG`, or `FULL`; a charging battery at 100% and at least 4.18 V is shown as full.
- The IP5306 keepalive pulse stays enabled while the firmware is in BLE or USB mode so the power module does not shut down under light load.
- The firmware does not use System OFF or deep sleep. After about 60 seconds without key activity, matrix scanning enters a shallow idle state and arms column GPIO interrupts. Any key press wakes the main loop and restores the 5 ms active scan interval.
- During shallow idle, the display backlight is turned off, display blanking is enabled, RGB status uses low-brightness breathing, LVGL runs at a reduced tick rate, and battery ADC sampling slows from 30 seconds to 120 seconds.
- BLE and USB links are kept alive during idle. BLE connections request a modest idle-friendly connection interval after connecting, and MODE switching releases all pressed keys before changing the active HID transport to avoid stuck keys.
- RGB brightness is reduced when battery level is below 20%.

## Runtime logs

- Useful `LOG_INF` lines remain for startup, mode changes, BLE connection state, matrix idle, key wakeup, display resume/idle, and RGB recovery.
- Per-key scan logs and encoder bring-up candidates are `LOG_DBG` to keep RTT readable during normal use. Raise the log level or temporarily change those lines back to `LOG_INF` when debugging key mapping.

## Bring-up checklist

- BLE mode: pair once, reboot the keyboard, confirm it reconnects without repeated security failures.
- USB mode: confirm Windows shows an HID keyboard and keypad input works in a text editor.
- Idle wakeup: wait for `Keyboard idle: matrix wakeup armed`, press any key, and confirm `Keyboard wakeup: active scan`.
- MODE switch: confirm USB disconnects BLE intentionally and BLE starts advertising again when switched back.
- Power: run on battery for at least 30 minutes and confirm the IP5306 does not shut down under idle load.

## Final validation checklist

- USB enumerates as an HID keyboard and numeric keypad input works.
- USB Boot Protocol and Report Protocol both continue to send keyboard reports.
- BLE can be discovered, paired, encrypted, disconnected, and reconnected.
- BLE HID keyboard reports and Consumer Control volume/mute reports work.
- MODE switching releases pressed keys and does not send reports to the old transport.
- NumLock toggles the local numeric/navigation layer and updates the display/RGB state.
- Encoder rotation sends volume up/down; encoder press sends mute.
- Battery voltage, percent, and `BAT`/`CHG`/`FULL` display correctly.
- After 60 seconds idle, the screen blanks, RGB breathes at low brightness, and a key press wakes the keyboard promptly.
- In BLE mode with no host connected, fast advertising changes to slow advertising after 300 seconds.
- USB host-control `PING`, `GET_STATUS`, `SYNC_TIME`, `GET_RGB`, and `SET_RGB` work through `tools/host_test.py`.
- BLE host-control service can be discovered by UUID `8f420000-7b6a-4f6a-9a52-4d41494b4244`; Request is `...0001...`, Response is `...0002...`.

## Hardware notes

The pin mapping is derived from `SCH_蓝牙小键盘_2026-03-28.pdf` and `E73-2G4M08S1C_UserManual_CN_v2.4.pdf`. If a bring-up test shows a swapped key row/column or an inverted MODE state, update `boards/me/ai_ble_keyboard/ai_ble_keyboard.dts`; application code reads all board pins from devicetree.
