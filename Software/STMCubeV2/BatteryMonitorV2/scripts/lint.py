#!/usr/bin/env python3
"""Run clang-tidy on hand-written embedded C in BatteryMonitorV2.

Uses compile_commands.json from a build directory (default: build/Debug, with
fallbacks to build/ci-Debug / build/Release / build/ci-Release). Targets only
the files we authored or substantially modified; CubeMX boilerplate, TinyUSB,
FreeRTOS, and HAL drivers are intentionally excluded.

Usage:
    python scripts/lint.py                          # lint with auto-detected build dir
    python scripts/lint.py --fix                    # apply --fix-errors where safe
    python scripts/lint.py --build-dir build/Debug  # explicit build dir
    python scripts/lint.py --warnings-as-errors     # CI mode: any warning fails

Exit codes:
    0 — clean
    1 — warnings or errors found
    2 — tooling / invocation error
"""
from __future__ import annotations

import argparse
import platform
import shutil
import subprocess
import sys
from pathlib import Path

# Files we own or substantially modified. Grow this list as we onboard more
# files to lint coverage. Auto-generated CubeMX files are intentionally absent.
LINT_FILES = [
    "Core/Src/app_freertos.c",
    "Core/Src/usb_descriptors.c",
    "Core/Src/stm32c0xx_it.c",
]

BUILD_DIR_FALLBACKS = ["build/Debug", "build/ci-Debug", "build/Release", "build/ci-Release"]


def print_install_help() -> None:
    """Print platform-specific install instructions for clang-tidy."""
    system = platform.system()
    print("error: clang-tidy not on PATH.", file=sys.stderr)
    print("", file=sys.stderr)
    print("Install options:", file=sys.stderr)
    if system == "Windows":
        print("  winget install LLVM.LLVM", file=sys.stderr)
        print("  or: choco install llvm", file=sys.stderr)
        print("  or: download ARM LLVM Embedded Toolchain from", file=sys.stderr)
        print("      https://github.com/ARM-software/LLVM-embedded-toolchain-for-Arm/releases", file=sys.stderr)
        print("      and add its bin/ to PATH.", file=sys.stderr)
        print("", file=sys.stderr)
        print("  After install, open a fresh terminal so PATH refreshes.", file=sys.stderr)
    elif system == "Linux":
        print("  sudo apt install clang-tidy   # Debian/Ubuntu", file=sys.stderr)
        print("  sudo dnf install clang-tools-extra   # Fedora", file=sys.stderr)
        print("  sudo pacman -S clang   # Arch", file=sys.stderr)
    elif system == "Darwin":
        print("  brew install llvm", file=sys.stderr)
        print("  Then add to PATH: $(brew --prefix llvm)/bin", file=sys.stderr)
    else:
        print(f"  (unknown platform: {system}) — install LLVM clang-tidy from your package manager", file=sys.stderr)


def find_clang_tidy(override: str | None) -> str | None:
    if override:
        return override if Path(override).is_file() else shutil.which(override)
    return shutil.which("clang-tidy")


def find_build_dir(override: str | None) -> Path | None:
    if override:
        p = Path(override)
        return p if (p / "compile_commands.json").is_file() else None
    for d in BUILD_DIR_FALLBACKS:
        p = Path(d)
        if (p / "compile_commands.json").is_file():
            return p
    return None


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--build-dir", help="dir containing compile_commands.json")
    p.add_argument("--tidy", help="clang-tidy executable (else: search PATH)")
    p.add_argument("--fix", action="store_true", help="apply --fix-errors")
    p.add_argument("--warnings-as-errors", action="store_true",
                   help="treat any warning as an error (CI mode)")
    args = p.parse_args()

    tidy = find_clang_tidy(args.tidy)
    if not tidy:
        print_install_help()
        return 2

    build_dir = find_build_dir(args.build_dir)
    if not build_dir:
        searched = args.build_dir or " | ".join(BUILD_DIR_FALLBACKS)
        print(f"error: no compile_commands.json found in: {searched}", file=sys.stderr)
        print("Run a CMake configure first (CMake Tools build button, or", file=sys.stderr)
        print("`cmake --preset Debug` from the firmware project root).", file=sys.stderr)
        return 2

    cmd = [tidy, "-p", str(build_dir)]
    if args.fix:
        cmd.append("--fix-errors")
    if args.warnings_as_errors:
        cmd.append("--warnings-as-errors=*")
    cmd.extend(LINT_FILES)

    print(f"$ clang-tidy -p {build_dir} [{len(LINT_FILES)} files]")
    result = subprocess.run(cmd)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
