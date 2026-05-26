# AI BLE Keyboard 硬件整理

本文档整理当前固件使用到的硬件信息。原理图来源为用户指定文件：

```text
d:\sz0107课件\05_ble_keyboard\260107_SZ\尚硅谷嵌入式项目之蓝牙键盘\2.资料\1.原理图\SCH_蓝牙小键盘_2026-03-28.pdf
```

仓库中的 [schematic_text.txt](../schematic_text.txt) 是原理图文本抽取结果。由于 PDF 文本编码存在乱码，中文标题仅作为参考，网络名和器件名以抽取文本、DTS 和实测结果共同确认。

## 主控

模块：

```text
E73-2G4M08S1C
```

主控芯片：

```text
nRF52840
```

Zephyr 板级 compatible：

```text
me,ai-ble-keyboard
```

Zephyr board target：

```text
ai_ble_keyboard/nrf52840
```

## GPIO 分配

| 功能 | 信号 | nRF52840 引脚 | DTS 配置 |
| --- | --- | --- | --- |
| MODE 开关 | MODE | P0.29 | `mode-gpios`，上拉，高电平 BLE，低电平 USB |
| EC11 A | EC11A | P0.10 | 上拉，低有效 |
| EC11 B | EC11B | P1.06 | 上拉，低有效 |
| BAT_ADC_EN | 电池采样使能 | P1.04 | 高有效输出 |
| RGB_PWR_EN | RGB 电源使能 | P0.13 | 高有效输出 |
| IP5306_WAKEUP | IP5306 KEY/wakeup | P0.22 | 低有效开漏输出 |
| SCREEN_BL | 屏幕背光 | P1.11 | 低有效 |
| BAT_ADC | 电池 ADC | AIN7 | ADC channel 7 |
| USB D- | USB | D- | nRF USBD |
| USB D+ | USB | D+ | nRF USBD |

## 键盘矩阵

矩阵为 6 行 4 列，行扫描低有效，列输入上拉低有效。

| 行 | 引脚 |
| --- | --- |
| ROW0 | P0.15 |
| ROW1 | P0.07 |
| ROW2 | P0.12 |
| ROW3 | P0.04 |
| ROW4 | P1.09 |
| ROW5 | P0.08 |

| 列 | 引脚 |
| --- | --- |
| COL0 | P0.05 |
| COL1 | P0.06 |
| COL2 | P0.26 |
| COL3 | P0.30 |

当前按键映射：

| 行/列 | COL0 | COL1 | COL2 | COL3 |
| --- | --- | --- | --- | --- |
| ROW0 | Esc | 无 | 无 | EC11 按下 |
| ROW1 | NumLock | `/` | `*` | `-` |
| ROW2 | `7` / Home | `8` / Up | `9` / PgUp | `+` |
| ROW3 | `4` / Left | `5` | `6` / Right | `+` |
| ROW4 | `1` / End | `2` / Down | `3` / PgDn | `+` |
| ROW5 | `0` / Ins | `.` / Del | 无 | Enter |

说明：

- NumLock 开启时为数字层。
- NumLock 关闭时为导航层。
- ROW0/COL3 是 EC11 旋钮按下，发送 Consumer Control Mute，不发送普通键盘 usage。

## EC11 编码器

| 功能 | 引脚 | 行为 |
| --- | --- | --- |
| A 相 | P0.10 | 旋转采样 |
| B 相 | P1.06 | 旋转采样 |
| 按下 | ROW0/COL3 | 静音 |

固件行为：

- 顺时针：Volume Up。
- 逆时针：Volume Down。
- 按下：Mute。

## RGB 灯

器件：

```text
WS2812B Mini / 兼容灯珠
```

DTS：

```text
&spi2 {
    led_strip: ws2812@0 {
        compatible = "worldsemi,ws2812-spi";
        chain-length = <24>;
    };
};
```

| 功能 | 引脚/配置 |
| --- | --- |
| RGB 数据 | SPI2 MOSI，对应 pinctrl |
| RGB 电源使能 | P0.13，高有效 |
| 灯珠数量 | 24 |
| 颜色顺序 | GRB |

## 屏幕

屏幕驱动：

```text
ST7789V
```

接口：

```text
SPI1 + MIPI DBI
```

| 功能 | 引脚 |
| --- | --- |
| CS | P0.02，低有效 |
| DC | P0.03，高有效 |
| RESET | P1.10，低有效 |
| BL | P1.11，低有效 |

DTS 当前配置：

```text
width = 320
height = 240
x-offset = 0
y-offset = 0
mdac = 0xa0
mipi-max-frequency = 8000000
```

应用层按有效区域 320 x 172、Y 偏移 34 进行布局。

## 电池检测

原理图信号：

```text
BAT_ADC
BAT_ADC_EN
```

固件逻辑：

- `BAT_ADC_EN` 拉高后等待采样稳定。
- 通过 SAADC AIN7 读取分压点。
- 按 2:1 分压换算电池电压。
- 4.2 V 映射为 100%。
- 3.3 V 映射为 0%。
- 空闲时采样间隔从 30 秒延长到 120 秒。

## IP5306 电源

器件：

```text
IP5306-I2C
```

相关原理图网络：

```text
IP5305T_WAKEUP
IP5305T_I2C_SCK
IP5305T_I2C_SDA
SYS_POWER
VBAT
VBUS
```

说明：原理图网络名中使用 `IP5305T_*`，实际器件标注为 `IP5306-I2C`。

固件配置：

| 项目 | 配置 |
| --- | --- |
| I2C 地址 | `0x75` |
| I2C 总线 | `i2c0` |
| KEY/wakeup | P0.22，低有效开漏 |
| 保活脉冲 | 每 15 秒拉低 300 ms |

启动时写入的寄存器：

| 寄存器 | 位 | 目的 |
| --- | --- | --- |
| `0x00` | `BIT(5)` | 使能 Boost |
| `0x00` | `BIT(1)` | 保持 Boost 输出 |
| `0x01` | `BIT(2)` | VIN 拔出后继续 Boost |
| `0x20` | `BIT(4)` | 使能充电 |

这个配置用于解决慢拔 USB 时 IP5306 误关机的问题。

## 存储分区

DTS 固定分区：

| 分区 | 地址 | 大小 |
| --- | ---: | ---: |
| `mcuboot` | `0x00000` | 48 KB |
| `image-0` | `0x0c000` | 472 KB |
| `image-1` | `0x82000` | 472 KB |
| `storage` | `0xf8000` | 32 KB |

`storage` 用于 Zephyr settings/NVS，保存 BLE bond 等信息。

## 调试建议

- 如果某个键不工作，优先查矩阵行列和 `src/keymap.c`。
- 如果 BLE 反复连接失败，优先删除电脑端旧蓝牙配对记录。
- 如果电池供电掉电，优先看 RTT 中 IP5306 寄存器写入日志，并测 P0.22 是否有低有效保活脉冲。
- 如果屏幕方向或偏移异常，优先检查 ST7789V 的 `mdac`、宽高和应用层有效区域。
