# BatteryMonitorV2

Firmware for the battery-charger-module's STM32C071 monitor board.

- **MCU:** STM32C071 (Cortex-M0+, 48 MHz), 24 KB SRAM, 64 KB flash
- **RTOS:** FreeRTOS 11.2 via CMSIS-RTOS V2 (X-CUBE-FREERTOS pack v1.5.0 — *not* the legacy built-in middleware)
- **USB:** Vendored TinyUSB 0.20.0 (CDC ACM), shares the same PA11/PA12 USB-FS port with DFU
- **Build:** STM32CubeIDE-bundled clang + cmake + ninja, driven by CubeMX 6.17
- **Local dev tooling:** WSL Ubuntu (lint / unit tests / coverage) — same env as CI

## Quick start

```powershell
# One-time setup (installs clang-tidy, Ceedling, gcovr, ARM LLVM toolchain in WSL)
scripts\setup-wsl.ps1

# Build firmware: CMake Tools extension build button, or F7

# Flash to board (board must be in DFU mode — hold BOOT0, tap RESET)
# Ctrl+Shift+B   -> runs "Flash (DFU)" task
# After flashing, tap RESET (without BOOT0) to exit DFU and run

# Lint
scripts\run-lint.ps1                 # or "Lint (clang-tidy, WSL)" task

# Unit tests
scripts\run-tests.ps1                # or "Unit tests (Ceedling, WSL)" task

# Coverage report
scripts\run-coverage.ps1             # or "Coverage (Ceedling gcov, WSL)" task

# Watch telemetry stream from a running board
python -m serial.tools.miniterm --list   # find the COM port
python -m serial.tools.miniterm COM7 115200
```

Most VS Code tasks live under **Ctrl+Shift+P → Tasks: Run Task**. The unit-test task is the default for `Tasks: Run Test Task`.

## Project layout

```
BatteryMonitorV2/
├── BatteryMonitorV2.ioc           # CubeMX project — hardware config source of truth
├── CMakeLists.txt                 # Project CMake (hand-edited; CubeMX-aware)
├── CMakePresets.json              # Debug/Release (starm-clang) + ci-Debug/ci-Release (arm-clang)
├── Core/
│   ├── Inc/                       # Headers (CubeMX-generated, plus our additions)
│   │   ├── main.h, gpio.h, ...    # CubeMX-generated
│   │   ├── direct_io.h            # hand-written
│   │   └── tusb_config.h          # TinyUSB user config
│   └── Src/                       # Sources
│       ├── main.c, gpio.c, ...    # CubeMX-generated (USER CODE blocks safe)
│       ├── direct_io.c            # hand-written: LED / relay / bleed / fan helpers
│       ├── usb_descriptors.c      # TinyUSB device descriptors
│       └── app_freertos.c         # FreeRTOS task bodies (USER CODE blocks)
├── Middlewares/
│   ├── Third_Party/FreeRTOS/      # CubeMX-managed
│   └── tinyusb/                   # Vendored TinyUSB (see VERSION.txt for bump procedure)
├── Drivers/                       # CubeMX-managed STM32C0xx HAL + CMSIS
├── cmake/
│   ├── stm32cubemx/CMakeLists.txt # CubeMX-regenerated — DO NOT EDIT
│   ├── starm-clang.cmake          # local Windows toolchain (ST bundle)
│   └── arm-clang.cmake            # CI toolchain (ARM LLVM Embedded)
├── scripts/                       # Dev workflow scripts
│   ├── setup-wsl.{sh,ps1}         # One-time WSL setup
│   ├── run-lint.{sh,ps1}          # clang-tidy
│   ├── run-tests.{sh,ps1}         # Ceedling test:all
│   ├── run-coverage.{sh,ps1}      # Ceedling gcov:all + Cobertura/HTML reports
│   ├── check_memory.py            # post-build memory budget check
│   └── telemetry_check.py         # serial regression smoke test
├── test/                          # Host-side unit tests (Ceedling + Unity + CMock)
│   ├── project.yml                # Ceedling config
│   ├── test_direct_io.c           # tests
│   └── support/                   # HAL stubs that swap in for Core/Inc/ during tests
└── CLAUDE.md                      # Conventions / gotchas (read by Claude Code)
```

## Build environment

CubeMX (6.17) drives code generation; STM32CubeIDE-bundled clang + cmake + ninja drive the build. The VS Code **CMake Tools** extension is the canonical build front-end — it sets PATH for the bundled tools automatically, so the build "just works" from the GUI.

Bundled tools live under `%CUBE_BUNDLE_PATH%` (typically `C:\Users\<user>\AppData\Local\stm32cube\bundles\`). **Versions drift over time** — discover the actually-used paths from the existing build cache:

```bash
grep -E "^CMAKE_(C_COMPILER|MAKE_PROGRAM|COMMAND|TOOLCHAIN_FILE):" build/<Preset>/CMakeCache.txt
```

Currently (as of last known good build):

- `cmake` — `bundles/cmake/4.3.1+st.1/bin/cmake.exe`
- `ninja` — `bundles/ninja/1.13.2+st.1/bin/ninja.exe`
- `starm-clang` toolchain — `bundles/st-arm-clang/21.1.1+st.7/bin/` (`starm-clang`, `starm-objcopy`, `starm-size`, etc.)
- Toolchain file: [cmake/starm-clang.cmake](cmake/starm-clang.cmake) — Cortex-M0+, **picolibc** C library, `.elf` output, `-T BatteryMonitorV2.ld` (a renamed local copy of CubeMX's `STM32C071XX_FLASH.ld`, with ld.lld + DFU-alignment fixes — see file header)
- Generator: Ninja
- Presets: `Debug` (`-Og -g3`) and `Release` (`-Oz -g0`) → `build/Debug/`, `build/Release/`

[CMakeLists.txt](CMakeLists.txt) adds a post-build `objcopy` to emit `BatteryMonitorV2.bin` alongside the `.elf` (the bin is what `STM32_Programmer_CLI` uses for DFU).

### Building from outside VS Code

The bundled tools aren't on `PATH` by default, but you can derive them from an existing build cache (provided that cache wasn't created by VS Code, see gotcha below). PowerShell:

```powershell
$cache = "build/Debug/CMakeCache.txt"   # use whichever was configured via CLI, not VS Code
$cmake = (((Select-String -Path $cache -Pattern "^CMAKE_COMMAND:").Line) -split "=",2)[1]
$ninja = (((Select-String -Path $cache -Pattern "^CMAKE_MAKE_PROGRAM:").Line) -split "=",2)[1]
$ar    = (((Select-String -Path $cache -Pattern "^CMAKE_AR:").Line) -split "=",2)[1]
$env:PATH = "$(Split-Path $ar);$(Split-Path $ninja);$env:PATH"

& $cmake --build build/Debug       # build
```

**Cache gotcha:** when VS Code's CMake Tools extension runs the configure, it writes `CMAKE_COMMAND:UNINITIALIZED=cube-cmake` (a wrapper that's not on PATH — only the extension knows how to resolve it). Trying to invoke that from a plain shell fails. The recipe above works only when the cache was created by a previous direct invocation of the bundled `cmake.exe`. If you're starting from a VS Code-configured cache, either do one CLI configure to overwrite `CMAKE_COMMAND` with an absolute path, or bootstrap from the bundle dir:

```powershell
$clang = (Get-ChildItem "$env:CUBE_BUNDLE_PATH\st-arm-clang" -Recurse -Filter starm-clang.exe | Select-Object -First 1).FullName
$ninja = (Get-ChildItem "$env:CUBE_BUNDLE_PATH\ninja"        -Recurse -Filter ninja.exe       | Select-Object -First 1).FullName
$cmake = (Get-ChildItem "$env:CUBE_BUNDLE_PATH\cmake"        -Recurse -Filter cmake.exe       | Select-Object -First 1).FullName
$env:PATH = "$(Split-Path $clang);$(Split-Path $ninja);$env:PATH"
& $cmake --preset Debug
& $cmake --build build/Debug
```

`CMAKE_AR` is used as a proxy for the toolchain `bin/` dir (its sibling is `starm-clang.exe`).

## Flash workflow — DFU only

No SWD programmer needed for routine flashing.

**One-time per chip:** set option byte `nBOOT_SEL=0` so the BOOT0 pin is honored.

```
STM32_Programmer_CLI -c port=SWD -ob nBOOT_SEL=0
```

After that, the BOOT0 button + reset drops the board into the ROM bootloader on the same USB port. Two ways to flash:

**1. CMake `flash` target** (preferred — auto-builds first, scriptable, visible in CMake Tools target dropdown):
```
cmake --build build/Debug --target flash
```
Defined in [CMakeLists.txt](CMakeLists.txt). Hardcodes `STM32_Programmer_CLI` at `C:/Program Files/.../STM32CubeProgrammer/bin/`; override via `-DSTM32_PROGRAMMER_CLI=<path>` if installed elsewhere.

**2. VS Code task** ([.vscode/tasks.json](.vscode/tasks.json)) — `Flash (DFU)`, default build task (Ctrl+Shift+B). Uses `${command:cmake.buildType}` to pick Debug or Release automatically. Does **not** rebuild — assumes you've already built via CMake Tools.

Either way: no `-hardRst`/`-rst` flags (those only work over SWD/JTAG). After flash completes, tap RESET (without BOOT0) to exit DFU and start the new firmware.

User flow: hold BOOT0 + tap RESET (board enters DFU on the same USB port) → run flash → tap RESET to run.

## USB stack — TinyUSB, not HAL

USB Device Library is **not** distributed for STM32C0 in any CubeMX pack (ST ships only Azure USBX/ThreadX for C0). We use **vendored TinyUSB 0.20.0** at [Middlewares/tinyusb/](Middlewares/tinyusb/) (trimmed to `src/{tusb.c,tusb.h,tusb_option.h}` + `common/` + `device/` + `osal/` + `class/cdc/` + `portable/st/stm32_fsdev/`, ~760 KB total).

Ownership model:
- **CubeMX keeps USB enabled** (Device_Only, no middleware) so `MX_USB_PCD_Init` → `HAL_PCD_MspInit` configures HSI48, USB clock mux, peripheral clock, and NVIC for us. HSI48 isn't visible in the clock tree when USB is off, so we leave USB on to keep it.
- `HAL_PCD_Init` runs at boot then sits unused — ~100 µs wasted, but harmless.
- TinyUSB owns the USB peripheral at runtime. `tud_init(0)` in the `usbDeviceTask` reprograms the registers. The `USB_DRD_FS_IRQHandler` USER CODE block in [Core/Src/stm32c0xx_it.c](Core/Src/stm32c0xx_it.c) hijacks the IRQ via `tud_int_handler(0); return;` — bypasses the HAL handler entirely.
- TinyUSB config: [Core/Inc/tusb_config.h](Core/Inc/tusb_config.h) (CDC ACM, `OPT_MCU_STM32C0`, `OPT_OS_FREERTOS`).
- USB descriptors: [Core/Src/usb_descriptors.c](Core/Src/usb_descriptors.c). VID/PID `0xCafe/0x4001` (TinyUSB example values — replace before any external release).

Relevant FreeRTOS tasks in [Core/Src/app_freertos.c](Core/Src/app_freertos.c):
- `usbDeviceTask` (priority AboveNormal): `tud_init(0)` then `tud_task()` loop. Blocks on TinyUSB's FreeRTOS queue between events.
- `telemetryTask` (priority Normal): emits `tick N\r\n` once per second when `tud_cdc_connected()`.

## Interrupt priorities

Cortex-M0+ has only 2 priority bits (0..3, lower = higher urgency) and **no `BASEPRI`** — FreeRTOS critical sections mask all interrupts via `PRIMASK`. The X-CUBE-FREERTOS pack enforces `LIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 0`, meaning any ISR with "Uses FreeRTOS functions" checked in CubeMX will be forced to priority 0. Don't fight it — priority differentiation only matters between non-FreeRTOS-aware ISRs.

- All user-facing IRQs (USB, I2C1, I2C2, USART1, EXTI4_15): priority 0, "Uses FreeRTOS functions" ✓
- SysTick / PendSV: priority 3 (FreeRTOS-owned, leave alone)
- TIM2 (FreeRTOS time base): priority 3 (FreeRTOS-owned)

## Local development setup (WSL)

Local lint / unit tests / coverage all run inside WSL Ubuntu so they use the same toolchain as CI. ST's `starm-clang` bundle doesn't ship `clang-tidy`/`gcov`/`ceedling`, and native Windows `clang-tidy` can't cross-compile arm-none-eabi.

**One-time setup:**
```powershell
scripts\setup-wsl.ps1
```

PowerShell wrapper around `wsl bash scripts/setup-wsl.sh`. Installs apt packages (`clang-tidy ninja-build cmake python3 ruby ruby-dev gcovr curl`), the `ceedling` gem, and downloads ARM LLVM Embedded Toolchain 19.1.5 to `/opt/llvm-arm`. ~5 minutes. Prompts for sudo password. Re-runnable, idempotent.

**Linux / macOS (no WSL needed):**
```
sudo apt install clang-tidy ninja-build cmake python3 curl ruby ruby-dev gcovr
sudo gem install ceedling
# Then download ARM LLVM Embedded Toolchain to /opt/llvm-arm — see setup-wsl.sh for the recipe.
```

## Static analysis (clang-tidy)

Configured via [.clang-tidy](.clang-tidy) at the firmware project root. Lints only the files we own or substantially modified (listed in `LINT_FILES` in [scripts/lint.py](scripts/lint.py)) — CubeMX boilerplate, TinyUSB, FreeRTOS, and HAL are excluded.

```powershell
scripts\run-lint.ps1                       # warnings as warnings
scripts\run-lint.ps1 --warnings-as-errors  # CI mode
scripts\run-lint.ps1 --fix                 # apply auto-fixes; review diff after
```

[scripts/run-lint.sh](scripts/run-lint.sh) configures the `ci-Debug` preset to emit `compile_commands.json`, extracts ARM clang's implicit include paths via `-v -E`, passes each as `--extra-arg=-isystem<dir>`, then delegates to [scripts/lint.py](scripts/lint.py). Configure is skipped if `build/ci-Debug/compile_commands.json` is fresher than `CMakeLists.txt` / `CMakePresets.json`.

**Integrations:**
- **CMake targets:** `cmake --build build/<preset> --target lint` (or `lint-fix`). Shows up in the CMake Tools target dropdown.
- **VS Code tasks:** "Lint (clang-tidy, WSL)" and "Lint (clang-tidy, WSL) --fix". The non-fix task has a problem matcher so warnings land in the Problems panel.
- **CI:** the `lint` job in [.github/workflows/firmware-build.yml](../../../.github/workflows/firmware-build.yml) runs essentially the same recipe with `--warnings-as-errors`. Fails the build on any warning.

## Unit tests (Ceedling)

Host-side unit tests with [Unity](https://www.throwtheswitch.org/unity) + [CMock](https://www.throwtheswitch.org/cmock) via [Ceedling](https://www.throwtheswitch.org/ceedling). Test sources live in [test/](test/); production code is compiled against host gcc with stub HAL headers from [test/support/](test/support/). CMock auto-generates mocks for `HAL_GPIO_WritePin`, `__HAL_TIM_SET_COMPARE`, etc. so tests assert which HAL the helper called with which args — no real hardware in the loop.

```powershell
scripts\run-tests.ps1                  # all tests
scripts\run-tests.ps1 test:direct_io   # just one test file
scripts\run-tests.ps1 clobber          # nuke build artifacts
```

**Integrations:**
- **CMake target:** `cmake --build build/Debug --target test_unit`.
- **VS Code task:** "Unit tests (Ceedling, WSL)". Set as default for the test group, so Ctrl+Shift+P → "Tasks: Run Test Task" runs it.
- **CI:** `test_unit` job in the workflow.

### How the test isolation works

Ceedling's [test/project.yml](test/project.yml) puts [test/support/](test/support/) at the front of the include path. When `direct_io.c` does `#include "main.h"`, it picks up the **stub** [test/support/main.h](test/support/main.h) (a few HAL prototypes + pin defines as constants) instead of the real Core/Inc/main.h (which would drag in the entire CMSIS chain that doesn't parse with host gcc). Same trick for [test/support/tim.h](test/support/tim.h) — exposes `__HAL_TIM_SET_COMPARE`/`__HAL_TIM_GET_AUTORELOAD` as ordinary functions instead of HAL macros, so CMock can intercept them. `htim1` storage lives in [test/support/test_globals.c](test/support/test_globals.c). Port "pointers" (`MCU_LED_GPIO_Port`, etc.) are real GPIO_TypeDef instances with unique `_id` values so CMock's default pointer-arg memcmp distinguishes them.

**Build root** is `test/build/` (in-project). We briefly tried `/tmp/` for a 5-7× speedup on WSL, but it broke `gcov`'s source-path resolution (`.gcda` records source paths relative to the build dir, and `/tmp/.../Core/Src/...` resolved nowhere) so the Cobertura XML came out empty and Codecov rejected it. Working coverage > faster runs — revisit via `-fprofile-prefix-map` if WSL fs perf becomes painful.

**To add a new module under test:**
1. Add the new `Core/Src/<module>.c` path to `LINT_FILES` in [scripts/lint.py](scripts/lint.py) (existing requirement for clang-tidy coverage).
2. Create `test/test_<module>.c` mirroring the pattern in `test_direct_io.c`. Ceedling auto-links the matching `Core/Src/<module>.c` by filename.
3. If the module uses HAL functions not already stubbed, add the prototype to [test/support/main.h](test/support/main.h) (or a peer header) and the corresponding `#include "mock_*.h"` to the test file.
4. If the module genuinely can't be host-tested (FreeRTOS task body, ISR forwarder, static descriptor tables), add it to `TEST_EXEMPT` in [scripts/check_tests.py](scripts/check_tests.py) **with a one-line reason** instead of step 2. The diff is reviewer-visible.

**Test-coverage enforcement.** [scripts/check_tests.py](scripts/check_tests.py) asserts every entry in `LINT_FILES` either has a matching `test_<module>.c` or is in `TEST_EXEMPT`. CI runs it as the **last** step of the `test_unit` job — after the Codecov upload — so a missing test fails the job loudly without suppressing the coverage data for everything else. The HTML artifact and Codecov upload still publish first; the red status comes from this check. Run locally with `python scripts/check_tests.py --list` to see the current manifest.

## Coverage

`ceedling gcov:all` runs the tests with `gcov` instrumentation and emits both HTML and Cobertura XML reports. Branch coverage is enabled (`:branches: TRUE` in `test/project.yml`). Test scaffolding (Unity, CMock, generated mocks/runners, support/ stubs) is excluded so the metric only reflects production code under test.

```powershell
scripts\run-coverage.ps1          # Windows → WSL
```
```
bash scripts/run-coverage.sh      # Linux / macOS / direct WSL shell
```

Outputs land at `test/build/artifacts/gcov/gcovr/`:
- `GcovCoverageResults.html` — browse in any browser
- `GcovCoverageCobertura.xml` — Codecov / GitLab / Azure DevOps format

**Integrations:**
- **CMake target:** `cmake --build build/Debug --target test_coverage`.
- **VS Code task:** "Coverage (Ceedling gcov, WSL)".
- **CI:** the `test_unit` job runs `ceedling gcov:all`, uploads the HTML report as a PR artifact (30-day retention), and pushes the Cobertura XML to **Codecov**.

### Viewing coverage on a PR

Four ways the report surfaces, in increasing detail:

1. **PR comment from the Codecov bot.** Appears within a minute of `test_unit` finishing. Shows project-vs-patch deltas and a per-file table with links into the dashboard. Click a filename to jump to that file's annotated source on app.codecov.io. Updates in place on each push.
2. **`codecov/patch` status check** in the PR's checks list. Fails when patch coverage falls below the `patch.target` set in [.codecov.yml](../../../.codecov.yml) (currently 70%). The `codecov/project` check is informational — it logs project drift but doesn't block merge.
3. **Inline annotations in "Files changed"** — uncovered new lines render with a ⚠ marker next to the diff, same UX as ESLint/clang-tidy annotations. Powered by `github_checks.annotations: true` in [.codecov.yml](../../../.codecov.yml). No per-reviewer setup required.
4. **HTML report as a workflow artifact.** Actions run → `coverage-html` → download the zip and open `GcovCoverageResults.html` for a local browseable view with line-by-line gutters (what's covered, partial, uncovered, branch-uncovered). Useful when you want to dig into a function without round-tripping through the dashboard.

**Dashboard:** [app.codecov.io/gh/tuffinmuffin/battery-cart](https://app.codecov.io/gh/tuffinmuffin/battery-cart) — sunburst, file tree, commit/PR history. The PR comment links here.

**Optional — richer inline view via browser extension.** The Codecov GitHub Checks annotations only mark uncovered lines. If you want full red/green gutters overlaid on every line of the diff, install the [Codecov browser extension](https://github.com/codecov/sourcegraph-codecov) (Chrome/Firefox). It's per-developer; the server-side annotations work without it.

**Codecov upload token** lives in repo secret `CODECOV_TOKEN`. The upload step has `fail_ci_if_error: true`, so a token rotation that isn't reflected in the secret will fail the `test_unit` job — rotate both ends together.

## Stack usage tracking

Three complementary signals for "do FreeRTOS tasks fit in their `stack_size` budgets?"

**1. Static report from `.su` files (target-accurate, fastest).** The firmware build emits per-function frame sizes via `-fstack-usage`. [scripts/stack_report.py](scripts/stack_report.py) parses every `.su` under the build dir, parses `arm-none-eabi-objdump -d`'s call graph from the ELF, and DFS-walks from each task entry function to compute worst-case stack depth.

```
python scripts/stack_report.py [--show-path] [--fail-pct 80]
cmake --build build/Debug --target stack_report          # cmake wrapper
```

Output is a per-task table with `WORST / BUDGET / USED%`. Tasks where TinyUSB/HAL dispatch via function pointers get an `indirect-calls` flag — the static graph can't follow those, so trust the runtime HWM below instead.

Task → `stack_size` mapping is hardcoded in the script (`TASK_BUDGETS`); update it when a task's attributes change.

**2. Runtime high-water mark over CDC (most accurate, on-target).** [Core/Src/ina238_task.c](Core/Src/ina238_task.c) emits `stk_free=<N>B` on each 1 Hz telemetry line via `osThreadGetStackSpace(NULL)`. That's the minimum free bytes ever observed since the task started — what the kernel actually saw. Pattern is easy to copy to other tasks; rip the print line once the budget is settled.

**3. Sentinel-fill in unit tests (regression detection, host-side).** [test/support/stack_measure.{h,c}](test/support/) runs a function under test on a pthread-allocated stack prefilled with `0xA5`, then scans for the first dirty byte. Caveats are spelled out in the header — short version: host frames, not ARM frames; pthread baseline is ~6 KB on glibc so always pair with an `run_empty` baseline and assert on the delta. Catches "somebody added a 2 KB local buffer" regressions; misses sub-baseline regressions.

See `test_stack_delta_of_read_reg16_is_modest` in [test/test_ina238.c](test/test_ina238.c) for the pattern.

## Host-side serial tooling

`pyserial` for serial work and automated tests:

```
pip install pyserial
python -m serial.tools.miniterm --list                  # find the COM port
python -m serial.tools.miniterm COM7 115200             # interactive (CDC baud ignored)
python scripts/telemetry_check.py [COM7]                # auto-detects if port omitted
```

[scripts/telemetry_check.py](scripts/telemetry_check.py) reads 5 s of telemetry and asserts the `tick N` counter is monotonic. Exit 0 = pass. Foundation for future regression tests.

## CI overview

[`.github/workflows/firmware-build.yml`](../../../.github/workflows/firmware-build.yml) at the repo root. Triggers on every push (any branch) and PR. Jobs:

| Job | What it does |
|---|---|
| `build (ci-Debug)` / `build (ci-Release)` | Cross-compile with ARM LLVM 19.1.5, fail if `--print-memory-usage` crosses 85% on RAM or FLASH ([scripts/check_memory.py](scripts/check_memory.py)). Upload `.elf`/`.bin`/`.map` as 30-day PR artifacts. |
| `lint` | apt-installed clang-tidy + ARM LLVM include paths, `--warnings-as-errors`. |
| `test_unit` | apt-installed Ruby + Ceedling + gcovr. Runs `ceedling gcov:all`. Uploads HTML coverage as PR artifact. Pushes Cobertura XML to Codecov for inline PR overlay. |

Concurrency group cancels in-flight runs when a newer commit pushes to the same ref.
