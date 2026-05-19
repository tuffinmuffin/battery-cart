#!/usr/bin/env bash
# One-time WSL setup for BatteryMonitorV2 local lint workflow.
#
# Mirrors the CI runner environment so `python3 scripts/lint.py` produces
# byte-identical output to GitHub Actions.
#
# Usage (from the BatteryMonitorV2 project root, in Windows PowerShell):
#     wsl bash scripts/setup-wsl.sh
#
# Will prompt for sudo password (apt + /opt). Re-runnable; skips work that's
# already done.

set -euo pipefail

LLVM_VERSION="${LLVM_VERSION:-19.1.5}"
INSTALL_DIR="${INSTALL_DIR:-/opt/llvm-arm}"

echo "==> [1/3] Installing apt packages (clang-tidy, cmake, ninja, python3, ruby, gcovr, curl)"
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential clang-tidy ninja-build cmake python3 curl ca-certificates \
    ruby ruby-dev gcc gcovr

echo ""
echo "==> [2/3] Installing Ceedling (Unity + CMock test framework)"
if command -v ceedling >/dev/null 2>&1; then
    echo "    already installed: $(ceedling version 2>&1 | head -1)"
else
    sudo gem install --no-document ceedling
fi

echo ""
echo "==> [3/3] ARM LLVM Embedded Toolchain $LLVM_VERSION at $INSTALL_DIR"
if [ -x "$INSTALL_DIR/bin/clang" ]; then
    echo "    already installed, skipping"
else
    cd /tmp
    echo "    downloading (~180 MB)..."
    curl -fsSL -o llvm-arm.tar.xz \
        "https://github.com/ARM-software/LLVM-embedded-toolchain-for-Arm/releases/download/release-${LLVM_VERSION}/LLVM-ET-Arm-${LLVM_VERSION}-Linux-x86_64.tar.xz"
    sudo mkdir -p "$INSTALL_DIR"
    echo "    extracting..."
    sudo tar -xJf llvm-arm.tar.xz -C "$INSTALL_DIR" --strip-components=1
    rm llvm-arm.tar.xz
fi

echo ""
echo "Verifying:"
"$INSTALL_DIR/bin/clang" --version | head -1
clang-tidy --version | head -1
ceedling version 2>&1 | head -1 | sed 's/^/Ceedling /'

echo ""
echo "Setup complete."
echo "  Lint:        wsl bash scripts/run-lint.sh"
echo "  Unit tests:  wsl bash scripts/run-tests.sh"
echo ""
echo "From CMake:    cmake --build build/Debug --target lint   (or --target test_unit)"
echo "From VS Code:  Ctrl+Shift+P -> 'Tasks: Run Task'"
