#!/usr/bin/env python3
"""Development helper for the desk controller over USB Serial/JTAG.

    tools/.venv/bin/python tools/devtool.py log [--seconds 10] [--reset]
    tools/.venv/bin/python tools/devtool.py cmd "stats" ["desk up" ...]
    tools/.venv/bin/python tools/devtool.py shot screen.png
    tools/.venv/bin/python tools/devtool.py tap 320 90

The port is auto-detected (first /dev/cu.usbmodem*), override with --port.
Opening the port does not reset the board (DTR/RTS are held inactive) unless
--reset is given.
"""

from __future__ import annotations

import argparse
import base64
import glob
import sys
import time
import zlib

import serial  # pyserial


def find_port() -> str:
    ports = sorted(glob.glob("/dev/cu.usbmodem*")) + sorted(glob.glob("/dev/ttyACM*"))
    if not ports:
        sys.exit("no USB serial port found")
    return ports[0]


def open_port(port: str, reset: bool = False) -> serial.Serial:
    # The ESP32-S3 USB-Serial-JTAG resets the chip when RTS is asserted while
    # DTR is not. Opening leaves both asserted (no reset); never drop DTR alone.
    ser = serial.Serial(port, 115200, timeout=0.05)
    if reset:
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
        ser.dtr = True
    return ser


def read_for(ser: serial.Serial, seconds: float, echo: bool = True, until: str | None = None) -> str:
    end = time.time() + seconds
    out = bytearray()
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            out += chunk
            if echo:
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
            if until and until.encode() in out:
                break
    return out.decode("utf-8", "replace")


def send_command(ser: serial.Serial, command: str, wait: float, echo: bool = True, until: str | None = None) -> str:
    ser.reset_input_buffer()
    ser.write((command + "\r\n").encode())
    ser.flush()
    return read_for(ser, wait, echo=echo, until=until)


def rgb565le_to_png(data: bytes, width: int, height: int, path: str) -> None:
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type none
        row = data[y * width * 2:(y + 1) * width * 2]
        for x in range(width):
            v = row[2 * x] | (row[2 * x + 1] << 8)
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            raw += bytes(((r * 527 + 23) >> 6, (g * 259 + 33) >> 6, (b * 527 + 23) >> 6))

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return len(payload).to_bytes(4, "big") + tag + payload + zlib.crc32(tag + payload).to_bytes(4, "big")

    header = width.to_bytes(4, "big") + height.to_bytes(4, "big") + bytes((8, 2, 0, 0, 0))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def cmd_shot(ser: serial.Serial, path: str) -> None:
    text = send_command(ser, "shot", 15.0, echo=False, until="SHOT-END")
    if "SHOT-BEGIN" not in text or "SHOT-END" not in text:
        sys.exit("screenshot failed:\n" + text[-500:])
    body = text[text.index("SHOT-BEGIN"):]
    lines = body.splitlines()
    _, width, height, fmt, length = lines[0].split()[:5]
    b64 = []
    crc_expected = None
    for line in lines[1:]:
        line = line.strip()
        if line.startswith("SHOT-END"):
            crc_expected = int(line.split()[1], 16)
            break
        b64.append(line)
    data = base64.b64decode("".join(b64))
    if len(data) != int(length):
        sys.exit(f"length mismatch {len(data)} != {length}")
    crc = zlib.crc32(data) & 0xFFFFFFFF
    if crc_expected is not None and crc != crc_expected:
        sys.exit(f"crc mismatch {crc:08x} != {crc_expected:08x}")
    rgb565le_to_png(data, int(width), int(height), path)
    print(f"saved {path} ({width}x{height})")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    sub = ap.add_subparsers(dest="action", required=True)
    p_log = sub.add_parser("log")
    p_log.add_argument("--seconds", type=float, default=10)
    p_log.add_argument("--reset", action="store_true")
    p_cmd = sub.add_parser("cmd")
    p_cmd.add_argument("commands", nargs="+")
    p_cmd.add_argument("--wait", type=float, default=1.5)
    p_shot = sub.add_parser("shot")
    p_shot.add_argument("path")
    p_tap = sub.add_parser("tap")
    p_tap.add_argument("x", type=int)
    p_tap.add_argument("y", type=int)
    p_tap.add_argument("--ms", type=int, default=80)
    args = ap.parse_args()

    ser = open_port(args.port or find_port(), reset=getattr(args, "reset", False))
    try:
        if args.action == "log":
            read_for(ser, args.seconds)
        elif args.action == "cmd":
            for c in args.commands:
                send_command(ser, c, args.wait)
        elif args.action == "shot":
            cmd_shot(ser, args.path)
        elif args.action == "tap":
            send_command(ser, f"tap {args.x} {args.y} {args.ms}", 1.0)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
