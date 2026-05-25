# AI BLE Keyboard Host Protocol

The same 64-byte command format is shared by both transports:

- USB HID Feature Report, Report ID `0x03`, total length `64` bytes.
- BLE custom GATT service, 64-byte Request/Response characteristics.

The hardware mode switch is authoritative. The host can read the current mode,
but cannot switch USB/BLE mode.

## Transport Selection

Recommended desktop app strategy:

1. Prefer USB HID when the USB control collection is available.
2. If USB is unavailable, scan BLE device name `AI_BLE_KEYBOARD` or the custom
   service UUID below.
3. After BLE connection, use the same `PING`, `GET_STATUS`, `SYNC_TIME`,
   `SET_RGB` protocol packets.

## Request

Host sends a Feature Report:

| Offset | Size | Meaning |
| --- | ---: | --- |
| 0 | 1 | Report ID, always `0x03` |
| 1 | 1 | Command |
| 2 | 1 | Sequence number, echoed by response |
| 3 | 1 | Payload length `N` |
| 4 | N | Payload |

## Response

Host reads Feature Report `0x03` after each request:

| Offset | Size | Meaning |
| --- | ---: | --- |
| 0 | 1 | Report ID, always `0x03` |
| 1 | 1 | Command |
| 2 | 1 | Sequence number |
| 3 | 1 | Status: `0` OK, `1` bad command, `2` bad length, `3` unsupported |
| 4 | 1 | Payload length `N` |
| 5 | N | Payload |

## Commands

| Command | Value | Direction | Payload |
| --- | ---: | --- | --- |
| `GET_FW_INFO` | `0x01` | host -> device | none |
| `GET_STATUS` | `0x02` | host -> device | none |
| `SYNC_TIME` | `0x03` | host -> device | `uint32 unix_time`, `int16 timezone_offset_min`, `uint8 flags` |
| `SET_RGB` | `0x04` | host -> device | `enable`, `r`, `g`, `b`, `brightness`, `idle_brightness` |
| `GET_RGB` | `0x05` | host -> device | none |
| `SET_MODE` | `0x06` | host -> device | unsupported by design |
| `PING` | `0x7f` | host -> device | none |

All integers are little endian. `brightness` and `idle_brightness` are `0..100`.

## `GET_STATUS` Response Payload

| Offset | Size | Meaning |
| --- | ---: | --- |
| 0 | 1 | Mode: `0` BLE, `1` USB, `2` OFF |
| 1 | 1 | Current mode connected: USB ready or BLE connected |
| 2 | 1 | NumLock layer: `1` NUM, `0` NAV |
| 3 | 1 | Idle/low power display state |
| 4 | 1 | Battery percent, `0xff` unknown |
| 5 | 2 | Battery millivolts, `0` unknown |
| 7 | 1 | Charge state: `0` unknown, `1` discharging, `2` charging, `3` full |

## Python hidapi Flow

1. Enumerate VID `0x1915`, PID `0xA107`.
2. Open the HID path whose `usage_page == 0xff00` and `usage == 0x0001`.
   Do not open the keyboard collection (`usage_page == 0x0001`, `usage == 0x0006`).
3. Send a 64-byte feature report with byte `0 = 0x03`.
4. Read a 64-byte feature report with report ID `0x03`.
5. Match response command and sequence number.

## BLE GATT Transport

Device name:

```text
AI_BLE_KEYBOARD
```

Service UUID:

```text
8f420000-7b6a-4f6a-9a52-4d41494b4244
```

Request Characteristic:

```text
8f420001-7b6a-4f6a-9a52-4d41494b4244
Properties: Write, Write Without Response
Payload: 64-byte request packet
```

Response Characteristic:

```text
8f420002-7b6a-4f6a-9a52-4d41494b4244
Properties: Read, Notify
Payload: 64-byte response packet
```

The custom control characteristics are readable/writable without requiring
attribute-level encryption, so the desktop app can discover and test the
control channel even before HID pairing is fully settled. The HID keyboard
service still uses its normal BLE security behavior.

The firmware configures ATT MTU/buffers for 64-byte writes and notifications.
With bleak, prefer:

1. Connect.
2. Subscribe to Response notifications.
3. Write exactly 64 bytes to Request.
4. Wait for the matching Response notification.
5. If notifications are not available, read the Response characteristic after
   writing the Request characteristic.

Minimal bleak exchange shape:

```python
SERVICE_UUID = "8f420000-7b6a-4f6a-9a52-4d41494b4244"
REQUEST_UUID = "8f420001-7b6a-4f6a-9a52-4d41494b4244"
RESPONSE_UUID = "8f420002-7b6a-4f6a-9a52-4d41494b4244"

request = bytes([...])  # 64 bytes, same format as USB
await client.start_notify(RESPONSE_UUID, on_response)
await client.write_gatt_char(REQUEST_UUID, request, response=True)
```
