#!/usr/bin/env python3
"""Minimal host-side protocol tester for AI BLE Keyboard.

Install dependency:
    pip install hidapi

Examples:
    python tools/host_test.py list
    python tools/host_test.py ping
    python tools/host_test.py fw
    python tools/host_test.py status
    python tools/host_test.py sync-time
    python tools/host_test.py set-rgb --enable 1 --rgb 0 24 0 --brightness 80
    python tools/host_test.py get-rgb
"""

from __future__ import annotations

import argparse
import datetime as dt
import struct
import sys

try:
    import hid
except ImportError:
    print("Missing dependency: pip install hidapi", file=sys.stderr)
    raise


VID = 0x1915
PID = 0xA107
REPORT_ID = 0x03
REPORT_SIZE = 64

CMD_GET_FW_INFO = 0x01
CMD_GET_STATUS = 0x02
CMD_SYNC_TIME = 0x03
CMD_SET_RGB = 0x04
CMD_GET_RGB = 0x05
CMD_SET_MODE = 0x06
CMD_PING = 0x7F

STATUS_TEXT = {
    0: "OK",
    1: "BAD_CMD",
    2: "BAD_LEN",
    3: "UNSUPPORTED",
}

MODE_TEXT = {
    0: "BLE",
    1: "USB",
    2: "OFF",
}

CHARGE_TEXT = {
    0: "UNKNOWN",
    1: "DISCHARGING",
    2: "CHARGING",
    3: "FULL",
}


class HostProtocolError(RuntimeError):
    pass


def device_text(item: dict) -> str:
    manufacturer = item.get("manufacturer_string") or ""
    product = item.get("product_string") or ""
    serial = item.get("serial_number") or ""
    usage_page = item.get("usage_page", 0)
    usage = item.get("usage", 0)
    interface = item.get("interface_number", -1)
    path = item.get("path", b"")
    if isinstance(path, bytes):
        path = path.decode(errors="replace")
    return (
        f"usage_page=0x{usage_page:04x} usage=0x{usage:04x} "
        f"interface={interface} product={product!r} "
        f"manufacturer={manufacturer!r} serial={serial!r}\n"
        f"  path={path}"
    )


def list_devices() -> list[dict]:
    return list(hid.enumerate(VID, PID))


def open_device(path: str | bytes | None = None) -> hid.device:
    dev = hid.device()
    if path is None:
        devices = list_devices()
        vendor = [
            item
            for item in devices
            if item.get("usage_page") == 0xFF00 and item.get("usage") == 0x0001
        ]
        if not vendor:
            raise HostProtocolError(
                "Vendor HID collection not found. Run `python tools\\host_test.py list`, "
                "confirm latest firmware is flashed, then unplug/replug USB."
            )
        path = vendor[0]["path"]

    dev.open_path(path)
    dev.set_nonblocking(False)
    return dev


def exchange(dev: hid.device, cmd: int, payload: bytes = b"", seq: int = 1) -> bytes:
    if len(payload) > REPORT_SIZE - 4:
        raise ValueError("payload too large")

    request = bytearray(REPORT_SIZE)
    request[0] = REPORT_ID
    request[1] = cmd
    request[2] = seq & 0xFF
    request[3] = len(payload)
    request[4 : 4 + len(payload)] = payload

    dev.send_feature_report(bytes(request))
    response = bytes(dev.get_feature_report(REPORT_ID, REPORT_SIZE))

    if len(response) < 5:
        raise HostProtocolError(f"short response: {response!r}")
    if response[0] != REPORT_ID:
        raise HostProtocolError(f"bad report id: {response[0]:#x}")
    if response[1] != cmd or response[2] != (seq & 0xFF):
        raise HostProtocolError(
            f"response mismatch: cmd={response[1]:#x} seq={response[2]}"
        )

    status = response[3]
    payload_len = response[4]
    if status != 0:
        raise HostProtocolError(f"device returned {STATUS_TEXT.get(status, status)}")
    return response[5 : 5 + payload_len]


def c_string(data: bytes) -> str:
    return data.split(b"\x00", 1)[0].decode("ascii", errors="replace")


def cmd_ping(dev: hid.device) -> None:
    payload = exchange(dev, CMD_PING)
    print(payload.decode("ascii", errors="replace"))


def cmd_list() -> None:
    devices = list_devices()
    if not devices:
        print(f"No HID devices found for VID=0x{VID:04x} PID=0x{PID:04x}")
        return

    for index, item in enumerate(devices):
        marker = " <- control" if (
            item.get("usage_page") == 0xFF00 and item.get("usage") == 0x0001
        ) else ""
        print(f"[{index}]{marker}")
        print(device_text(item))


def cmd_fw(dev: hid.device) -> None:
    payload = exchange(dev, CMD_GET_FW_INFO)
    proto_major, proto_minor, fw_major, fw_minor, fw_patch, report_id, report_size = (
        payload[:7]
    )
    board = c_string(payload[8:32])
    build = c_string(payload[32:58])
    print(f"protocol: {proto_major}.{proto_minor}")
    print(f"firmware: {fw_major}.{fw_minor}.{fw_patch}")
    print(f"report: id=0x{report_id:02x} size={report_size}")
    print(f"board: {board}")
    print(f"build: {build}")


def cmd_status(dev: hid.device) -> None:
    payload = exchange(dev, CMD_GET_STATUS)
    mode = payload[0]
    connected = bool(payload[1])
    numlock = bool(payload[2])
    idle = bool(payload[3])
    battery = payload[4]
    battery_mv = struct.unpack_from("<H", payload, 5)[0]
    charge = payload[7]

    print(f"mode: {MODE_TEXT.get(mode, mode)}")
    print(f"connected: {connected}")
    print(f"layer: {'NUM' if numlock else 'NAV'}")
    print(f"idle: {idle}")
    print(f"battery: {'unknown' if battery == 0xFF else str(battery) + '%'}")
    print(f"voltage: {'unknown' if battery_mv == 0 else str(battery_mv) + 'mV'}")
    print(f"charge: {CHARGE_TEXT.get(charge, charge)}")


def cmd_sync_time(dev: hid.device) -> None:
    now = dt.datetime.now().astimezone()
    unix_time = int(now.timestamp())
    offset_min = int(now.utcoffset().total_seconds() // 60)
    flags = 0
    payload = struct.pack("<IhB", unix_time, offset_min, flags)
    exchange(dev, CMD_SYNC_TIME, payload)
    print(f"synced: {now.isoformat(timespec='seconds')}")


def cmd_set_rgb(dev: hid.device, args: argparse.Namespace) -> None:
    r, g, b = args.rgb
    payload = bytes(
        [
            1 if args.enable else 0,
            r & 0xFF,
            g & 0xFF,
            b & 0xFF,
            max(0, min(100, args.brightness)),
            max(0, min(100, args.idle_brightness)),
        ]
    )
    exchange(dev, CMD_SET_RGB, payload)
    print("rgb updated")


def cmd_get_rgb(dev: hid.device) -> None:
    payload = exchange(dev, CMD_GET_RGB)
    print(f"enabled: {bool(payload[0])}")
    print(f"rgb: {payload[1]} {payload[2]} {payload[3]}")
    print(f"brightness: {payload[4]}")
    print(f"idle_brightness: {payload[5]}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("list")
    sub.add_parser("ping")
    sub.add_parser("fw")
    sub.add_parser("status")
    sub.add_parser("sync-time")
    sub.add_parser("get-rgb")

    rgb = sub.add_parser("set-rgb")
    rgb.add_argument("--enable", type=int, default=1, choices=[0, 1])
    rgb.add_argument("--rgb", type=int, nargs=3, default=[0, 24, 0])
    rgb.add_argument("--brightness", type=int, default=100)
    rgb.add_argument("--idle-brightness", type=int, default=40)

    return parser


def main() -> int:
    args = build_parser().parse_args()

    if args.command == "list":
        cmd_list()
        return 0

    dev = open_device()
    try:
        if args.command == "ping":
            cmd_ping(dev)
        elif args.command == "fw":
            cmd_fw(dev)
        elif args.command == "status":
            cmd_status(dev)
        elif args.command == "sync-time":
            cmd_sync_time(dev)
        elif args.command == "set-rgb":
            cmd_set_rgb(dev, args)
        elif args.command == "get-rgb":
            cmd_get_rgb(dev)
        else:
            raise AssertionError(args.command)
    finally:
        dev.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
