#!/usr/bin/env python3
"""Estimate per-task stack usage from .su files + ELF call graph.

Combines two artefacts the firmware build already produces:
  - `.su` files (one per compilation unit) emitted by `-fstack-usage`, with
    per-function frame sizes for the real target compiler (arm-clang).
  - `arm-none-eabi-objdump -d <elf>` disassembly, parsed to extract the
    static call graph from `bl <target>` instructions.

For each FreeRTOS task entry function the script DFS-walks the call graph,
sums frame sizes along the heaviest path, and compares against the task's
configured stack size. Recursion is detected and reported. Indirect calls
(`blx rX`) and function-pointer dispatch (HAL weak callbacks, FreeRTOS
xTaskCreate target) are invisible to static disassembly — for those, the
script emits a warning when a task's entry function has any indirect-call
sites.

Exit codes:
    0 — every task fits in its budget
    1 — at least one task exceeds budget OR had analysis warnings
    2 — tooling / invocation error
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

# Task entry function → configured stack_size in bytes. Stay in sync with
# `osThreadAttr_t` instances in source. The script can't extract these
# automatically because FreeRTOS dispatches via function pointer, so the
# linkage between task entry and stack budget isn't visible to objdump.
TASK_BUDGETS = {
    "StartDefaultTask":   128 * 4,
    "StartUsbDeviceTask": 256 * 4,
    "StartTelemetryTask": 256 * 4,
    "Ina238TaskBody":     256 * 4,
}

BUILD_DIR_FALLBACKS = ["build/Release", "build/Debug", "build/ci-Release", "build/ci-Debug" ]

# arm-none-eabi-objdump search order. Bundled ST toolchain ships as
# `starm-objdump` next to `starm-clang`.
OBJDUMP_CANDIDATES = [
    "arm-none-eabi-objdump",
    "starm-objdump",
]


def find_objdump(override: str | None) -> str | None:
    if override:
        return override if Path(override).is_file() else shutil.which(override)
    for name in OBJDUMP_CANDIDATES:
        path = shutil.which(name)
        if path:
            return path
    # Fall back to scanning the bundled STM32 cube toolchain when running on
    # Windows outside VS Code (the bundle isn't on PATH). Search order:
    # 1) $CUBE_BUNDLE_PATH (set by VS Code's CMake Tools extension)
    # 2) %LOCALAPPDATA%\stm32cube\bundles (default Windows install location)
    # 3) ~/.local/share/stm32cube/bundles (Linux/macOS default, if anyone
    #    ever uses the bundle there instead of system arm-none-eabi-objdump)
    bundle_candidates = [
        os.environ.get("CUBE_BUNDLE_PATH"),
        os.path.join(os.environ.get("LOCALAPPDATA", ""), "stm32cube", "bundles")
            if os.environ.get("LOCALAPPDATA") else None,
        os.path.expanduser("~/.local/share/stm32cube/bundles"),
    ]
    for root in bundle_candidates:
        if not root or not Path(root).is_dir():
            continue
        cands = list(Path(root).glob("st-arm-clang/*/bin/starm-objdump*"))
        if cands:
            # Prefer highest version (last after lexical sort works for "21.x").
            return str(sorted(cands)[-1])
    return None


def find_build_dir(override: str | None) -> Path | None:
    if override:
        p = Path(override)
        return p if p.is_dir() else None
    for d in BUILD_DIR_FALLBACKS:
        p = Path(d)
        if p.is_dir():
            return p
    return None


def find_elf(build_dir: Path) -> Path | None:
    elves = list(build_dir.glob("*.elf"))
    return elves[0] if elves else None


SU_LINE_RE = re.compile(r"^.*?:\d+:(?P<func>[A-Za-z_][\w.]*)\s+(?P<size>\d+)\s+(?P<qual>\S+)")


def parse_su_files(build_dir: Path) -> dict[str, tuple[int, str]]:
    """Walk build_dir for *.su, return {function -> (frame_size, qualifier)}."""
    frames: dict[str, tuple[int, str]] = {}
    for su in build_dir.rglob("*.su"):
        for line in su.read_text(errors="replace").splitlines():
            m = SU_LINE_RE.match(line)
            if not m:
                continue
            func = m.group("func")
            size = int(m.group("size"))
            qual = m.group("qual")
            # If the same symbol appears in multiple TUs, keep the largest.
            # Static functions get suffixed by the linker so this is rare.
            if func not in frames or size > frames[func][0]:
                frames[func] = (size, qual)
    return frames


# Match a function header from objdump -d: "00008440 <function_name>:"
FUNC_HEADER_RE = re.compile(r"^[0-9a-f]+ <([^>]+)>:")
# Match a direct call: "8006be0:  bl  0x8007508 <relay_disable> @ imm = ...".
# llvm-objdump emits the target with `0x` prefix; gas-style omits it.
# We only count `bl`/`blx imm` because they leave a return address (= real
# call with stack growth). `b <target>` is an untaken-link branch — could be
# a tail call or intra-function loop; safer to skip than to overcount loops.
DIRECT_CALL_RE = re.compile(r"\s(?:bl|blx)\s+(?:0x)?[0-9a-fA-F]+\s+<([^>+]+)>")
# Match an indirect call via register: "  8006400: blx r3". `bx rN` is
# almost always a return or computed-goto from a jump table — not a call.
INDIRECT_CALL_RE = re.compile(r"\sblx\s+r\d+")


def parse_call_graph(objdump: str, elf: Path) -> tuple[dict[str, set[str]], set[str]]:
    """Return ({caller -> {callees}}, {functions with indirect-call sites})."""
    try:
        out = subprocess.check_output(
            [objdump, "-d", "--no-show-raw-insn", str(elf)],
            text=True, errors="replace",
        )
    except subprocess.CalledProcessError as e:
        print(f"error: objdump failed: {e}", file=sys.stderr)
        sys.exit(2)

    edges: dict[str, set[str]] = {}
    indirect: set[str] = set()
    current: str | None = None
    for line in out.splitlines():
        m = FUNC_HEADER_RE.match(line)
        if m:
            current = m.group(1)
            edges.setdefault(current, set())
            continue
        if current is None:
            continue
        m = DIRECT_CALL_RE.search(line)
        if m:
            target = m.group(1).strip()
            # objdump sometimes emits "callee+0x4" for tail-jump targets;
            # split on '+' / '-' just in case.
            target = re.split(r"[+\-]", target)[0]
            if target != current:  # ignore self
                edges[current].add(target)
            continue
        # bx lr is a return, not a call — only blx and computed bx are.
        if "bx\tlr" in line or " bx lr" in line:
            continue
        if INDIRECT_CALL_RE.search(line):
            indirect.add(current)
    return edges, indirect


def worst_case_stack(
    root: str,
    edges: dict[str, set[str]],
    frames: dict[str, tuple[int, str]],
    visiting: set[str] | None = None,
    cache: dict[str, tuple[int, list[str]]] | None = None,
) -> tuple[int, list[str], bool]:
    """DFS from root, return (max_stack_bytes, worst_path, recursion_seen)."""
    if visiting is None:
        visiting = set()
    if cache is None:
        cache = {}
    if root in cache and root not in visiting:
        size, path = cache[root]
        return size, path, False
    if root in visiting:
        return 0, [root + " (recursion)"], True
    own = frames.get(root, (0, "?"))[0]
    visiting.add(root)
    best_callee_stack = 0
    best_callee_path: list[str] = []
    recursion = False
    for callee in edges.get(root, set()):
        sub, sub_path, rec = worst_case_stack(callee, edges, frames, visiting, cache)
        recursion = recursion or rec
        if sub > best_callee_stack:
            best_callee_stack = sub
            best_callee_path = sub_path
    visiting.discard(root)
    total = own + best_callee_stack
    path = [f"{root} (+{own})"] + best_callee_path
    cache[root] = (total, path)
    return total, path, recursion


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build-dir", help="dir containing .su files + ELF")
    ap.add_argument("--objdump", help="path to arm-none-eabi-objdump")
    ap.add_argument("--show-path", action="store_true",
                    help="print the worst-case call path for each task")
    ap.add_argument("--fail-pct", type=float, default=80.0,
                    help="fail if any task exceeds this percentage of its budget")
    args = ap.parse_args()

    objdump = find_objdump(args.objdump)
    if not objdump:
        print("error: objdump not found.", file=sys.stderr)
        print("  Searched PATH for: " + ", ".join(OBJDUMP_CANDIDATES),
              file=sys.stderr)
        print("  Searched bundle: $CUBE_BUNDLE_PATH, %LOCALAPPDATA%\\stm32cube\\bundles",
              file=sys.stderr)
        print("Fix options:", file=sys.stderr)
        print("  1) Run from VS Code's CMake-Tools terminal (CUBE_BUNDLE_PATH set)",
              file=sys.stderr)
        print("  2) $env:CUBE_BUNDLE_PATH=\"$env:LOCALAPPDATA\\stm32cube\\bundles\"",
              file=sys.stderr)
        print("  3) python scripts/stack_report.py --objdump <full-path-to-starm-objdump.exe>",
              file=sys.stderr)
        return 2

    build_dir = find_build_dir(args.build_dir)
    if not build_dir:
        print(f"error: no build dir found. Tried: {' | '.join(BUILD_DIR_FALLBACKS)}",
              file=sys.stderr)
        return 2

    elf = find_elf(build_dir)
    if not elf:
        print(f"error: no .elf in {build_dir}", file=sys.stderr)
        return 2

    frames = parse_su_files(build_dir)
    edges, indirect = parse_call_graph(objdump, elf)

    print(f"$ stack_report.py  build_dir={build_dir}  elf={elf.name}  "
          f"{len(frames)} .su frames  {len(edges)} symbols in call graph")
    print()
    print(f"{'TASK':<22} {'WORST':>8} {'BUDGET':>8} {'USED':>7}  NOTES")

    failed = False
    for task, budget in TASK_BUDGETS.items():
        if task not in edges:
            print(f"{task:<22} {'?':>8} {budget:>8} {'?':>7}  not found in ELF (LTO/inline?)")
            failed = True
            continue
        worst, path, recursion = worst_case_stack(task, edges, frames)
        pct = (worst * 100.0) / budget if budget else 0.0
        notes = []
        if recursion:
            notes.append("RECURSION")
        if any(f.split(" ")[0] in indirect for f in path):
            notes.append("indirect-calls")
        if pct > args.fail_pct:
            notes.append(f">{args.fail_pct:.0f}%")
            failed = True
        print(f"{task:<22} {worst:>8} {budget:>8} {pct:>6.1f}%  {', '.join(notes)}")
        if args.show_path:
            for step in path:
                print(f"    {step}")
    print()
    if failed:
        print("FAIL: at least one task exceeds budget or analysis was incomplete",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
