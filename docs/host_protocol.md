# AI BLE Keyboard 上位机协议

本协议用于上位机和键盘固件交换控制信息。USB 和 BLE 使用同一套 64 字节数据包格式。

## 传输通道

### USB HID Feature Report

USB 信息：

```text
VID = 0x1915
PID = 0xA107
Report ID = 0x03
Report Size = 64
```

上位机应打开 Vendor HID 集合：

```text
usage_page = 0xff00
usage      = 0x0001
```

不要打开系统键盘集合：

```text
usage_page = 0x0001
usage      = 0x0006
```

### BLE GATT

设备名：

```text
AI_BLE_KEYBOARD
```

自定义 Service UUID：

```text
8f420000-7b6a-4f6a-9a52-4d41494b4244
```

Request Characteristic：

```text
8f420001-7b6a-4f6a-9a52-4d41494b4244
Properties: Write, Write Without Response
Payload: 64-byte request packet
```

Response Characteristic：

```text
8f420002-7b6a-4f6a-9a52-4d41494b4244
Properties: Read, Notify
Payload: 64-byte response packet
```

BLE 控制特征当前不强制属性级加密，方便上位机先发现和测试控制链路。HID 键盘服务仍保持正常 BLE 安全行为。

## 上位机连接策略

推荐策略：

1. 如果 USB HID Vendor 集合可用，优先使用 USB。
2. 如果 USB 不可用，再扫描 BLE。
3. BLE 扫描匹配设备名 `AI_BLE_KEYBOARD` 或自定义 Service UUID。
4. BLE 连接后订阅 Response Notify。
5. 写 64 字节 Request。
6. 等待匹配的 Response Notify。
7. 如果 Notify 不可用，写 Request 后读取 Response。

注意：当键盘已经作为 BLE HID 连接到 Windows 时，系统扫描结果可能不稳定。上位机应扫描多轮，每轮 3 到 5 秒，并允许通过已知地址或已配对设备重连。

## 请求包格式

Host 发送 64 字节请求。

| 偏移 | 长度 | 含义 |
| --- | ---: | --- |
| 0 | 1 | Report ID，固定 `0x03` |
| 1 | 1 | Command |
| 2 | 1 | Sequence，响应原样返回 |
| 3 | 1 | Payload 长度 `N` |
| 4 | N | Payload |
| 4 + N | 剩余 | 填 0 |

## 响应包格式

Device 返回 64 字节响应。

| 偏移 | 长度 | 含义 |
| --- | ---: | --- |
| 0 | 1 | Report ID，固定 `0x03` |
| 1 | 1 | Command |
| 2 | 1 | Sequence |
| 3 | 1 | Status |
| 4 | 1 | Payload 长度 `N` |
| 5 | N | Payload |
| 5 + N | 剩余 | 填 0 |

Status：

| 值 | 名称 | 含义 |
| ---: | --- | --- |
| `0x00` | `OK` | 成功 |
| `0x01` | `BAD_CMD` | 未知命令 |
| `0x02` | `BAD_LEN` | Payload 长度错误 |
| `0x03` | `UNSUPPORTED` | 命令存在但不支持 |

所有多字节整数均为 little endian。

## 命令表

| 命令 | 值 | 方向 | Payload |
| --- | ---: | --- | --- |
| `GET_FW_INFO` | `0x01` | Host -> Device | 无 |
| `GET_STATUS` | `0x02` | Host -> Device | 无 |
| `SYNC_TIME` | `0x03` | Host -> Device | `uint32 unix_time`, `int16 timezone_offset_min`, `uint8 flags` |
| `SET_RGB` | `0x04` | Host -> Device | `enable`, `r`, `g`, `b`, `brightness`, `idle_brightness` |
| `GET_RGB` | `0x05` | Host -> Device | 无 |
| `SET_MODE` | `0x06` | Host -> Device | 不支持，模式由硬件拨动开关决定 |
| `PING` | `0x7f` | Host -> Device | 无 |

## `PING`

请求 Payload：无。

响应 Payload：

```text
PONG
```

## `GET_FW_INFO`

响应 Payload 长度 58 字节。

| 偏移 | 长度 | 含义 |
| --- | ---: | --- |
| 0 | 1 | 协议主版本 |
| 1 | 1 | 协议次版本 |
| 2 | 1 | 固件主版本 |
| 3 | 1 | 固件次版本 |
| 4 | 1 | 固件补丁版本 |
| 5 | 1 | Report ID |
| 6 | 1 | Report Size |
| 7 | 1 | 保留 |
| 8 | 24 | 板名字符串，NUL 结尾 |
| 32 | 26 | 构建时间字符串，NUL 结尾 |

当前固件版本：

```text
0.1.0
```

## `GET_STATUS`

响应 Payload：

| 偏移 | 长度 | 含义 |
| --- | ---: | --- |
| 0 | 1 | Mode：`0` BLE，`1` USB，`2` OFF |
| 1 | 1 | 当前模式是否连接：USB ready 或 BLE connected |
| 2 | 1 | NumLock 层：`1` NUM，`0` NAV |
| 3 | 1 | Idle 状态 |
| 4 | 1 | 电量百分比，`0xff` 表示未知 |
| 5 | 2 | 电池电压 mV，`0` 表示未知 |
| 7 | 1 | 充电状态：`0` unknown，`1` discharging，`2` charging，`3` full |

## `SYNC_TIME`

请求 Payload：

| 偏移 | 长度 | 类型 | 含义 |
| --- | ---: | --- | --- |
| 0 | 4 | `uint32` | Unix 时间戳 |
| 4 | 2 | `int16` | 时区偏移，单位分钟 |
| 6 | 1 | `uint8` | flags，当前保留 |

固件收到后更新屏幕显示时间。设备完全断电后不会保存该时间。

## `SET_RGB`

请求 Payload：

| 偏移 | 长度 | 含义 |
| --- | ---: | --- |
| 0 | 1 | enable，`0` 使用固件状态灯效，`1` 使用上位机颜色 |
| 1 | 1 | R |
| 2 | 1 | G |
| 3 | 1 | B |
| 4 | 1 | active brightness，范围 `0..100` |
| 5 | 1 | idle brightness，范围 `0..100` |

## `GET_RGB`

响应 Payload：

| 偏移 | 长度 | 含义 |
| --- | ---: | --- |
| 0 | 1 | enable |
| 1 | 1 | R |
| 2 | 1 | G |
| 3 | 1 | B |
| 4 | 1 | active brightness |
| 5 | 1 | idle brightness |

## Python hidapi 示例

```python
import hid

VID = 0x1915
PID = 0xA107
REPORT_ID = 0x03
REPORT_SIZE = 64
CMD_PING = 0x7f

devs = hid.enumerate(VID, PID)
ctrl = next(d for d in devs if d["usage_page"] == 0xff00 and d["usage"] == 0x0001)

dev = hid.device()
dev.open_path(ctrl["path"])

req = bytearray(REPORT_SIZE)
req[0] = REPORT_ID
req[1] = CMD_PING
req[2] = 1
req[3] = 0

dev.send_feature_report(bytes(req))
resp = bytes(dev.get_feature_report(REPORT_ID, REPORT_SIZE))
print(resp)
```

## Python bleak 示例

```python
SERVICE_UUID = "8f420000-7b6a-4f6a-9a52-4d41494b4244"
REQUEST_UUID = "8f420001-7b6a-4f6a-9a52-4d41494b4244"
RESPONSE_UUID = "8f420002-7b6a-4f6a-9a52-4d41494b4244"

def on_response(_, data):
    print(bytes(data))

await client.start_notify(RESPONSE_UUID, on_response)
await client.write_gatt_char(REQUEST_UUID, bytes(request_64), response=True)
```
