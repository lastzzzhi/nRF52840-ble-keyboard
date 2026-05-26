# AI BLE Keyboard 蓝牙数字小键盘

这是一个基于 **nRF Connect SDK 3.2.3** 的 nRF52840 蓝牙/USB 双模数字小键盘固件工程。目标板为：

```text
ai_ble_keyboard/nrf52840
```

硬件原理图来源：

```text
SCH_蓝牙小键盘_2026-03-28.pdf
```

硬件引脚、矩阵、IP5306 电源逻辑整理见 [docs/hardware.md](docs/hardware.md)。

## 当前功能

- USB HID Keyboard，Windows 可枚举为键盘。
- BLE HID Keyboard，设备名 `AI_BLE_KEYBOARD`。
- 6 x 4 矩阵数字小键盘。
- NumLock 本地层切换：数字层 / 导航层。
- EC11 编码器：顺时针音量加，逆时针音量减，按下静音。
- WS2812 RGB 灯效。
- ST7789V 彩屏状态显示，屏幕物理分辨率 320 x 240，当前有效显示区域为 320 x 172，Y 偏移 34。
- IP5306-I2C 电源/电池状态读取与轻载保活。
- 低功耗浅空闲：不进入 System OFF，不关闭 BLE/USB 链路。
- 上位机控制协议：
  - USB HID Feature Report
  - BLE 自定义 GATT Service

## 构建和烧录

推荐使用 VS Code 的 nRF Connect 插件：

1. 打开本工程目录。
2. SDK 选择 `D:\ncs\v3.2.3`。
3. 添加 Build Configuration。
4. Board 选择 `ai_ble_keyboard/nrf52840`。
5. Build 后 Flash。

当前工作区验证过的命令行构建方式：

```powershell
& 'D:\ncs\toolchains\fd21892d0f\opt\bin\ninja.exe' -C build\AI_BLE_KEYBOARD
```

生成的固件位于：

```text
build\AI_BLE_KEYBOARD\zephyr\zephyr.elf
build\AI_BLE_KEYBOARD\zephyr\zephyr.hex
```

如果用 VS Code 插件烧录，日志中看到类似下面内容表示烧录成功：

```text
Board(s) with serial number(s) ... flashed successfully.
```

## USB 信息

USB 设备信息：

```text
VID: 0x1915
PID: 0xA107
Product: AI BLE Keyboard
Manufacturer: ZEPHYR
```

USB HID 集合：

| 用途 | Usage Page | Usage | 说明 |
| --- | ---: | ---: | --- |
| 键盘 | `0x0001` | `0x0006` | 系统键盘输入 |
| Consumer Control | `0x000c` | `0x0001` | 音量/静音 |
| 上位机控制 | `0xff00` | `0x0001` | 64 字节 Feature Report |

上位机测试脚本：

```powershell
python tools\host_test.py list
python tools\host_test.py ping
python tools\host_test.py status
python tools\host_test.py fw
python tools\host_test.py sync-time
python tools\host_test.py get-rgb
python tools\host_test.py set-rgb --enable 1 --rgb 0 24 0 --brightness 80
```

依赖：

```powershell
pip install hidapi
```

## BLE 信息

BLE 设备名：

```text
AI_BLE_KEYBOARD
```

BLE 服务：

| 服务 | UUID |
| --- | --- |
| HID over GATT | 标准 HOGP |
| Battery Service | 标准 BAS |
| 上位机控制 Service | `8f420000-7b6a-4f6a-9a52-4d41494b4244` |

自定义 GATT 特征：

| Characteristic | UUID | 方向 |
| --- | --- | --- |
| Request | `8f420001-7b6a-4f6a-9a52-4d41494b4244` | 上位机 Write 64 字节 |
| Response | `8f420002-7b6a-4f6a-9a52-4d41494b4244` | 固件 Notify/Read 64 字节 |

当前 BLE 连接/广播策略：

- 最大 BLE 连接数：2。
- HID 客户端数：2。
- 切到 BLE 模式后开启 300 秒快广播窗口。
- 快广播窗口结束后，如果仍未连接，会进入慢广播以降低功耗。
- BLE HID 连接成功后，如果还有空连接槽，会继续恢复广播，供上位机连接自定义 GATT 控制链路。
- 切到 USB 模式时停止 BLE 广播，并主动断开 BLE 连接。

协议细节见 [docs/host_protocol.md](docs/host_protocol.md)。

## 按键布局

矩阵为 6 行 4 列。第一行第四列是 EC11 旋钮按键，不作为普通矩阵键输出，按下发送静音。

数字层：

```text
Esc        --       --       EC11
NumLock   /        *        -
7         8        9        +
4         5        6        +
1         2        3        +
0         .        --       Enter
```

导航层：

```text
Esc        --       --       EC11
NumLock   /        *        -
Home      Up       PgUp     +
Left      5        Right    +
End       Down     PgDn     +
Ins       Del      --       Enter
```

## RGB 状态

默认灯光状态：

| 状态 | 颜色 |
| --- | --- |
| USB 已连接 | 绿色 |
| USB 未连接 | 红色 |
| BLE 已连接 | 蓝色偏青 |
| BLE 未连接/搜索 | 蓝色 |
| NumLock 关闭，也就是导航层 | 橙色 |
| 电量低于 20% | 在当前颜色基础上降低亮度 |
| 空闲 | 当前颜色低亮度呼吸 |

上位机可以通过 `SET_RGB` 临时覆盖颜色和亮度。

## 屏幕显示

屏幕型号按 ST7789V 配置：

- 物理分辨率：320 x 240
- 当前有效显示区域：320 x 172
- Y 偏移：34

显示内容：

- 当前时间和日期
- USB/BLE 模式
- 连接状态
- NumLock 层状态
- 电池百分比
- 电池供电/充电/满电状态

时间来源：

- 上位机发送 `SYNC_TIME` 后使用上位机同步时间。
- 未同步时使用固件构建时间加设备运行时间。
- 设备完全断电后没有 RTC 备份电源，时间不会长期保持准确。

## 电源和低功耗

电池电量：

- 4.2 V 映射为 100%。
- 3.3 V 映射为 0%。
- ADC 通过原理图中的 `BAT_ADC` 电量检测电路读取。
- 读取前拉高 `BAT_ADC_EN`，采样后关闭。

IP5306 逻辑：

- 固件启动时通过 I2C 配置 IP5306，使 VIN 拔出后继续保持 Boost 输出。
- `P0.22` 连接 IP5306 唤醒/KEY 相关网络，当前配置为低有效开漏。
- MCU 每 15 秒拉低 KEY/wakeup 线 300 ms，作为轻载保活兜底。
- 这个修复用于避免慢拔 USB 时 IP5306 误判轻载/按键状态后关机。

低功耗策略：

- 固件不使用 System OFF。
- 约 60 秒无按键后进入浅空闲。
- 浅空闲时矩阵扫描降频并打开列 GPIO 唤醒。
- 屏幕背光关闭，LVGL tick 降低。
- RGB 进入低亮度呼吸。
- BLE/USB 链路保持。
- 任意按键唤醒后恢复主动扫描、屏幕和灯效。

## 运行日志

使用 RTT 查看日志。常见关键日志：

```text
AI BLE Keyboard starting
IP5306 reg 0x00 |= ...
IP5306 reg 0x01 |= ...
IP5306 reg 0x20 |= ...
Bluetooth fast advertising started
Bluetooth connected
USB protocol changed to Report Protocol
Keyboard idle: matrix wakeup armed
Keyboard wakeup: active scan
```

如果 BLE 反复连接/断开，通常优先检查电脑端旧配对记录，删除后重新配对。

## 验证清单

- USB 模式能在记事本输入数字、小数点、加号、回车。
- USB NumLock LED 状态能回传到固件。
- USB Boot Protocol / Report Protocol 切换后仍能输入。
- BLE 模式电脑/手机能发现 `AI_BLE_KEYBOARD`。
- BLE 能配对、加密连接、断开后重连。
- BLE 键盘输入正常。
- BLE 旋钮音量/静音正常。
- BLE Battery Service 可见。
- 上位机 USB 控制通道 `PING / GET_STATUS / SYNC_TIME / GET_RGB / SET_RGB` 正常。
- 上位机 BLE GATT 控制通道能发现自定义 Service 并收发 64 字节协议包。
- 切换 USB/BLE 模式时旧通道按键会释放，不粘键。
- 电池供电下慢拔 USB 不再黑屏掉电。
- 空闲后按键能唤醒屏幕和灯效。
