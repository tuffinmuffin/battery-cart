#!/usr/bin/env python3
"""Memory budget check for the STM32C071 BatteryMonitorV2 firmware.

Reads `llvm-size` (or `arm-none-eabi-size`) output and fails if RAM or FLASH
usage exceeds the configured thresholds. Designed for CI invocation.

Also writes a markdown summary table to $GITHUB_STEP_SUMMARY when present,
which renders nicely in the GitHub Actions PR check page.

Usage:
    python3 scripts/check_memory.py <elf> [--ram-limit PCT] [--flash-limit PCT]

Exit codes:
    0 — within budget
    1 — exceeded a budget
    2 — invocation / tooling error
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

# STM32C071x8 / STM32C071xB: 24 KB SRAM, 64 KB flash
FLASH_TOTAL_B = 64 * 1024
RAM_TOTAL_B = 24 * 1024


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("elf", help="path to the firmware .elf")
    p.add_argument("--ram-limit", type=float, default=85.0,
                   help="fail when RAM usage exceeds this percent (default 85)")
    p.add_argument("--flash-limit", type=float, default=85.0,
                   help="fail when FLASH usage exceeds this percent (default 85)")
    args = p.parse_args()

    # llvm-size: ARM LLVM Embedded Toolchain (CI), arm-none-eabi-size: GCC,
    # starm-size: ST's CubeIDE-bundled toolchain (local Windows builds).
    size_bin = (shutil.which("llvm-size") or
                shutil.which("arm-none-eabi-size") or
                shutil.which("starm-size"))
    if size_bin is None:
        print("error: no size tool on PATH (looked for llvm-size, "
              "arm-none-eabi-size, starm-size)", file=sys.stderr)
        return 2

    try:
        out = subprocess.check_output([size_bin, args.elf], text=True)
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        print(f"error: size tool failed: {e}", file=sys.stderr)
        return 2

    # Expected output (Berkeley format):
    #    text    data     bss     dec     hex filename
    #   40000     100   15000   55100    d740 BatteryMonitorV2.elf
    lines = [ln for ln in out.strip().splitlines() if ln.strip()]
    if len(lines) < 2:
        print(f"error: unexpected size output:\n{out}", file=sys.stderr)
        return 2

    try:
        fields = lines[1].split()
        text, data, bss = int(fields[0]), int(fields[1]), int(fields[2])
    except (IndexError, ValueError) as e:
        print(f"error: could not parse size output: {e}\n{out}", file=sys.stderr)
        return 2

    flash_used = text + data
    ram_used = data + bss

    flash_pct = flash_used / FLASH_TOTAL_B * 100.0
    ram_pct = ram_used / RAM_TOTAL_B * 100.0

    print(f"  FLASH: {flash_used:>6} / {FLASH_TOTAL_B} B  ({flash_pct:5.2f}%, limit {args.flash_limit}%)")
    print(f"  RAM:   {ram_used:>6} / {RAM_TOTAL_B} B  ({ram_pct:5.2f}%, limit {args.ram_limit}%)")

    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8") as f:
            f.write("### Memory usage\n\n")
            f.write("| Region | Used | Total | % used | Limit |\n")
            f.write("|---|---:|---:|---:|---:|\n")
            f.write(f"| FLASH | {flash_used} B | {FLASH_TOTAL_B} B | {flash_pct:.2f}% | {args.flash_limit}% |\n")
            f.write(f"| RAM   | {ram_used} B | {RAM_TOTAL_B} B | {ram_pct:.2f}% | {args.ram_limit}% |\n")

    over = []
    if flash_pct > args.flash_limit:
        over.append("FLASH")
    if ram_pct > args.ram_limit:
        over.append("RAM")

    if over:
        print(f"FAIL: exceeded budget: {', '.join(over)}", file=sys.stderr)
        return 1

    print("OK: within budget")
    return 0


if __name__ == "__main__":
    sys.exit(main())
