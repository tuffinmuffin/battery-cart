"""Smoke-test the BatteryMonitorV2 CDC telemetry stream.

Reads lines from the given port for ~5 s and asserts they match `tick <n>\\r\\n`
with a monotonically increasing counter. Exits 0 on pass, 1 on fail.

Usage:
    python scripts/telemetry_check.py [PORT]

If PORT is omitted, lists available ports and picks the first one whose
description mentions "STMicroelectronics" or "Virtual COM".
"""
import re
import sys
import time

import serial
from serial.tools import list_ports

PATTERN = re.compile(rb"^tick (\d+)\r?\n?$")
WINDOW_S = 5.0
MIN_TICKS = 3


def autodetect_port() -> str | None:
    for p in list_ports.comports():
        desc = (p.description or "") + " " + (p.manufacturer or "")
        if "STMicroelectronics" in desc or "Virtual COM" in desc or "BatteryMonitor" in desc:
            return p.device
    return None


def main(argv: list[str]) -> int:
    port = argv[1] if len(argv) > 1 else autodetect_port()
    if port is None:
        print("no port given and no STM CDC device found; available:", file=sys.stderr)
        for p in list_ports.comports():
            print(f"  {p.device}: {p.description}", file=sys.stderr)
        return 2

    print(f"opening {port}")
    with serial.Serial(port, 115200, timeout=1.5) as ser:
        ser.reset_input_buffer()
        deadline = time.monotonic() + WINDOW_S
        last = None
        seen = 0
        while time.monotonic() < deadline:
            line = ser.readline()
            if not line:
                continue
            m = PATTERN.match(line)
            if not m:
                print(f"unexpected line: {line!r}", file=sys.stderr)
                return 1
            n = int(m.group(1))
            if last is not None and n != last + 1:
                print(f"non-monotonic: {last} -> {n}", file=sys.stderr)
                return 1
            last = n
            seen += 1
        if seen < MIN_TICKS:
            print(f"only saw {seen} ticks in {WINDOW_S:.1f} s", file=sys.stderr)
            return 1
        print(f"OK: {seen} ticks, last={last}")
        return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
