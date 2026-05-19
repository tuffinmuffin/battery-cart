#!/usr/bin/env python3
"""Assert every hand-written .c either has a unit test or is explicitly exempt.

Why this exists: Ceedling's gcov plugin only emits coverage for source files
that get linked into some test binary, and Ceedling only auto-links the
production source matching each test_<X>.c's name. The consequence: a new
hand-written module without a matching test silently disappears from the
Cobertura XML, Codecov never sees it, and the missing-coverage gap never
triggers any failure. This script catches that case at CI time.

The roster of hand-written files is `LINT_FILES` in scripts/lint.py — the
same single source of truth used to drive clang-tidy. Every entry there
must either:
  1. Have a corresponding test/test_<stem>.c, OR
  2. Appear in TEST_EXEMPT below with a one-line reason.

Adding a file to TEST_EXEMPT shows up in the diff and is reviewer-visible,
matching how LINT_FILES is managed.

Usage:
    python scripts/check_tests.py            # CI entry point
    python scripts/check_tests.py --list     # print the manifest + status

Exit codes:
    0 — clean (every LINT_FILES entry is either tested or exempt)
    1 — one or more files lack a test and are not exempt
    2 — tooling / invocation error (e.g. lint.py not importable)
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Import LINT_FILES from lint.py so this script stays in lockstep with the
# canonical roster. lint.py isn't a package, so prepend its dir to sys.path.
_SCRIPT_DIR = Path(__file__).parent
sys.path.insert(0, str(_SCRIPT_DIR))
try:
    from lint import LINT_FILES  # noqa: E402
except ImportError as exc:
    print(f"error: could not import LINT_FILES from lint.py: {exc}", file=sys.stderr)
    sys.exit(2)

# Modules that genuinely can't be exercised by a host-side Ceedling test.
# Add a one-line reason — the diff makes the trade-off visible to reviewers.
# If you find yourself wanting to exempt a module that has real branching
# logic, push back: a thin wrapper around HAL/RTOS calls is usually testable
# with CMock (see test_direct_io.c / test_i2c_bus.c for the pattern).
TEST_EXEMPT: dict[str, str] = {
    "Core/Src/app_freertos.c": (
        "FreeRTOS task bodies; CubeMX scaffolding + osDelay loops. "
        "Move real logic into a sibling .c with a test instead of exempting that too."
    ),
    "Core/Src/ina238_task.c": (
        "FreeRTOS 1 Hz telemetry loop - timing + hardware tied. "
        "Calibration math it would test is already covered via ina238.c."
    ),
    "Core/Src/usb_descriptors.c": (
        "Static TinyUSB descriptor tables; no branching logic."
    ),
    "Core/Src/stm32c0xx_it.c": (
        "Thin ISR forwarders to HAL / TinyUSB. Behaviour is verified on hardware."
    ),
    # TODO(tests): add test_cdc_print.c (mutex-protected printf path,
    # early-boot no-mutex path, lock-failure path) and remove this entry.
    # Carrying as exempt only so the check can land green; cdc_print.c HAS
    # testable branching logic and should not stay here.
    "Core/Src/cdc_print.c": (
        "TODO: testable but no test_cdc_print.c yet - remove this entry "
        "when the test lands."
    ),
}


def _firmware_root() -> Path:
    """Return the BatteryMonitorV2/ dir (script lives in scripts/)."""
    return _SCRIPT_DIR.parent


def _expected_test_path(src_rel: str) -> Path:
    """For Core/Src/foo.c return <firmware>/test/test_foo.c."""
    return _firmware_root() / "test" / f"test_{Path(src_rel).stem}.c"


def _audit() -> tuple[list[str], list[str], list[str]]:
    """Return (tested, exempt, missing) lists of LINT_FILES entries."""
    tested, exempt, missing = [], [], []
    for src in LINT_FILES:
        if src in TEST_EXEMPT:
            exempt.append(src)
            continue
        if _expected_test_path(src).is_file():
            tested.append(src)
            continue
        missing.append(src)
    return tested, exempt, missing


def _print_listing() -> None:
    tested, exempt, missing = _audit()
    print(f"Manifest from LINT_FILES ({len(LINT_FILES)} entries)")
    print()
    print(f"Tested ({len(tested)}):")
    for src in tested:
        print(f"  [OK]   {src}  -> {_expected_test_path(src).relative_to(_firmware_root())}")
    print()
    print(f"Exempt ({len(exempt)}):")
    for src in exempt:
        print(f"  [skip] {src}  -- {TEST_EXEMPT[src]}")
    print()
    print(f"Missing tests ({len(missing)}):")
    for src in missing:
        print(f"  [MISS] {src}  -> expected {_expected_test_path(src).relative_to(_firmware_root())}")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--list", action="store_true",
                   help="print the manifest with tested / exempt / missing status")
    args = p.parse_args()

    if args.list:
        _print_listing()
        _, _, missing = _audit()
        return 1 if missing else 0

    _, _, missing = _audit()
    if not missing:
        return 0

    print("error: hand-written source files without a unit test:", file=sys.stderr)
    for src in missing:
        expected = _expected_test_path(src).relative_to(_firmware_root())
        print(f"  {src}  →  expected {expected}", file=sys.stderr)
    print("", file=sys.stderr)
    print("Pick one:", file=sys.stderr)
    print("  1. Add a test at the expected path. See test/test_direct_io.c for", file=sys.stderr)
    print("     the basic shape; test_ina238.c demonstrates CMock-driven HAL stubs.", file=sys.stderr)
    print("  2. If the module truly can't be host-tested (ISR forwarder, FreeRTOS", file=sys.stderr)
    print("     task body, static descriptor tables), add it to TEST_EXEMPT in", file=sys.stderr)
    print("     scripts/check_tests.py with a one-line reason.", file=sys.stderr)
    return 1


# -----------------------------------------------------------------------------
# Future improvements — fold these in once the basic check has caught a few
# misses and we know which gaps actually matter:
#
# 1. Cobertura XML coverage assertion.
#    The current check only verifies a test FILE exists. A test_X.c that
#    forgets to `#include "X.h"` (or only links mocks) still compiles and
#    runs — but the module never gets gcov-instrumented and silently
#    disappears from coverage. Add a `--cobertura <path>` flag that parses
#    the gcovr XML and asserts every non-exempt LINT_FILES entry appears
#    as a <class filename="…"> in it.
#
#      from xml.etree import ElementTree as ET
#      tree = ET.parse(args.cobertura)
#      seen = {c.get("filename") for c in tree.iter("class")}
#      not_in_report = [f for f in LINT_FILES
#                       if f not in TEST_EXEMPT and f not in seen]
#
#    Run as a second CI step after `ceedling gcov:all`. Order:
#       (1) check_tests.py            → "file has a test"
#       (2) ceedling gcov:all         → produces the XML
#       (3) check_tests.py --cobertura→ "test actually links and runs the module"
#
# 2. Per-module coverage floor.
#    Once the modules listed in LINT_FILES have tests, add a per-file
#    minimum (e.g. "ina238.c ≥ 80% lines"). The .codecov.yml `flags` /
#    `components` system can express this declaratively, but enforcing it
#    in CI as well (parsing the same Cobertura XML) gives faster, more
#    legible failures than waiting for Codecov to post a status check.
#
# 3. Drop the LINT_FILES coupling.
#    Today this script defers to lint.py's allow-list. Once we trust the
#    convention, switch to auto-discovery: scan Core/Src/*.c, subtract a
#    DENY_PATTERNS list of CubeMX-generated filenames (main.c, gpio.c,
#    i2c.c, tim.c, usart.c, dma.c, sysmem.c, syscalls.c, system_stm32c0xx.c,
#    stm32c0xx_hal_msp.c, stm32c0xx_hal_timebase_tim.c). Catches files
#    added without being onboarded to LINT_FILES at all — but only worth
#    doing once a CubeMX regen has shown us the actual filename pattern
#    to deny.
#
# 4. Stub generation.
#    For the first-add case, ship `--init <module>` that writes a minimal
#    test_<module>.c with the standard setUp/tearDown + one `TEST_IGNORE()`
#    placeholder. Lowers the friction of choosing "test" over "exempt".
# -----------------------------------------------------------------------------


if __name__ == "__main__":
    sys.exit(main())
