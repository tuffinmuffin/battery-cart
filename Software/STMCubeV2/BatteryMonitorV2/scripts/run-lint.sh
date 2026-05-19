#!/usr/bin/env bash
# Run clang-tidy via the WSL/Linux toolchain mirror of CI.
#
# Prerequisites (one-time): `wsl bash scripts/setup-wsl.sh`.
#
# This script configures the ci-Debug preset to produce compile_commands.json
# (idempotent — skips if it already exists and is fresh), extracts the ARM
# clang's implicit include path list, and runs scripts/lint.py with each one
# as `--extra-arg=-isystem<path>` so system clang-tidy understands the
# arm-none-eabi target's picolibc headers.
#
# Passes any extra args through to lint.py (e.g. --fix, --warnings-as-errors).

set -euo pipefail

LLVM_ARM="${LLVM_ARM:-/opt/llvm-arm}"
BUILD_DIR="${BUILD_DIR:-build/ci-Debug}"

# Sanity checks
if [ ! -x "$LLVM_ARM/bin/clang" ]; then
    echo "error: ARM LLVM Embedded Toolchain not found at $LLVM_ARM" >&2
    echo "Run first: wsl bash scripts/setup-wsl.sh" >&2
    exit 2
fi
if ! command -v clang-tidy >/dev/null 2>&1; then
    echo "error: clang-tidy not on PATH" >&2
    echo "Run first: wsl bash scripts/setup-wsl.sh" >&2
    exit 2
fi
if ! command -v cmake >/dev/null 2>&1; then
    echo "error: cmake not on PATH" >&2
    echo "Run first: wsl bash scripts/setup-wsl.sh" >&2
    exit 2
fi

# Configure ci-Debug if compile_commands.json is missing or stale.
# Configure-only (no build), so the (READONLY) linker-script issue does not
# fire — that affects linking, not parse.
if [ ! -f "$BUILD_DIR/compile_commands.json" ] || \
   [ CMakeLists.txt -nt "$BUILD_DIR/compile_commands.json" ] || \
   [ CMakePresets.json -nt "$BUILD_DIR/compile_commands.json" ]; then
    echo "==> Configuring ci-Debug for compile_commands.json"
    export PATH="$LLVM_ARM/bin:$PATH"
    cmake --preset ci-Debug
fi

# Extract clang's implicit include search path for our target. System
# clang-tidy doesn't know where arm-none-eabi's picolibc headers live;
# pass them explicitly via --extra-arg=-isystem<dir>.
echo "==> Extracting ARM include paths from $LLVM_ARM/bin/clang"
INCLUDES=$("$LLVM_ARM/bin/clang" --target=arm-none-eabi -mcpu=cortex-m0plus \
           -v -E -x c /dev/null 2>&1 | \
           awk '/<\.\.\.> search starts here:/{f=1;next} /End of search list/{f=0} f && /^ /' | \
           sed 's/^ *//')
EXTRA=()
for p in $INCLUDES; do
    EXTRA+=("--extra-arg=-isystem$p")
done

echo "==> Running clang-tidy via scripts/lint.py"
exec python3 scripts/lint.py --build-dir "$BUILD_DIR" "${EXTRA[@]}" "$@"
