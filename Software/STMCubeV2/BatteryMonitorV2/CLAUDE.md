# BatteryMonitorV2

STM32C071 board for the battery charger module. FreeRTOS 11.2 via CMSIS-RTOS V2 (X-CUBE-FREERTOS pack v1.5.0 — *not* the legacy built-in middleware). 24 KB SRAM, 64 KB flash. Single USB-FS port on PA11/PA12, used for both DFU programming and CDC telemetry to the host.

## Build environment

CubeMX (6.17) drives code generation; STM32CubeIDE-bundled clang + cmake + ninja drive the build. The VS Code **CMake Tools** extension is the canonical build front-end — it sets PATH for the bundled tools automatically, so the build "just works" from the GUI.

Bundled tools live under `%CUBE_BUNDLE_PATH%` (typically `C:\Users\<user>\AppData\Local\stm32cube\bundles\`). **Versions drift** — discover the actually-used paths from the existing build cache:

```bash
grep -E "^CMAKE_(C_COMPILER|MAKE_PROGRAM|COMMAND|TOOLCHAIN_FILE):" build/<Preset>/CMakeCache.txt
```

Currently:
- `cmake` — `bundles/cmake/4.3.1+st.1/bin/cmake.exe`
- `ninja` — `bundles/ninja/1.13.2+st.1/bin/ninja.exe`
- `starm-clang` toolchain — `bundles/st-arm-clang/21.1.1+st.7/bin/` (`starm-clang`, `starm-objcopy`, `starm-size`, etc.)
- Toolchain file: [cmake/starm-clang.cmake](cmake/starm-clang.cmake) — Cortex-M0+, **picolibc** C library, `.elf` output, `-T STM32C071XX_FLASH.ld`
- Generator: Ninja
- Presets: `Debug` (`-Og -g3`) and `Release` (`-Oz -g0`) → `build/Debug/`, `build/Release/`

**Building from outside VS Code** (e.g. for a Claude verification run): the bundled tools aren't on `PATH` by default, but you can derive them from an existing build cache (provided that cache wasn't created by VS Code, see gotcha below). PowerShell:

```powershell
$cache = "build/Debug/CMakeCache.txt"   # use whichever was configured via CLI, not VS Code
$cmake = (((Select-String -Path $cache -Pattern "^CMAKE_COMMAND:").Line) -split "=",2)[1]
$ninja = (((Select-String -Path $cache -Pattern "^CMAKE_MAKE_PROGRAM:").Line) -split "=",2)[1]
$ar    = (((Select-String -Path $cache -Pattern "^CMAKE_AR:").Line) -split "=",2)[1]
$env:PATH = "$(Split-Path $ar);$(Split-Path $ninja);$env:PATH"

& $cmake --build build/Debug       # build
```

**Cache gotcha:** when VS Code's CMake Tools extension runs the configure, it writes `CMAKE_COMMAND:UNINITIALIZED=cube-cmake` (the wrapper, not the bundled binary). Trying to invoke that from a plain shell fails because `cube-cmake` isn't on PATH — only the extension knows how to resolve it. The recipe above works only when the cache was created by a previous direct invocation of the bundled `cmake.exe`. If you're starting from a VS Code-configured cache, either use the bootstrap below (which discovers tools from the bundle dir directly) or do one CLI configure to overwrite `CMAKE_COMMAND` with an absolute path.

`CMAKE_AR` is used as a proxy for the toolchain `bin/` dir (its sibling is `starm-clang.exe`).

If no build cache exists yet (clean checkout), bootstrap by enumerating the bundle dir once:

```powershell
$clang = (Get-ChildItem "$env:CUBE_BUNDLE_PATH\st-arm-clang" -Recurse -Filter starm-clang.exe | Select-Object -First 1).FullName
$ninja = (Get-ChildItem "$env:CUBE_BUNDLE_PATH\ninja"        -Recurse -Filter ninja.exe       | Select-Object -First 1).FullName
$cmake = (Get-ChildItem "$env:CUBE_BUNDLE_PATH\cmake"        -Recurse -Filter cmake.exe       | Select-Object -First 1).FullName
$env:PATH = "$(Split-Path $clang);$(Split-Path $ninja);$env:PATH"
& $cmake --preset Debug
& $cmake --build build/Debug
```

Project-level [CMakeLists.txt](CMakeLists.txt) adds a post-build `objcopy` to emit `BatteryMonitorV2.bin` alongside the `.elf` (the bin is what `STM32_Programmer_CLI` uses for DFU).

## Flash workflow — DFU only (no SWD programmer needed for routine flashing)

One-time per chip: option byte `nBOOT_SEL=0` so the BOOT0 pin is honored.

```
STM32_Programmer_CLI -c port=SWD -ob nBOOT_SEL=0
```

After that, the BOOT0 button + reset drops the board into the ROM bootloader on the same USB port. Two ways to flash:

**1. CMake `flash` target** (preferred — auto-builds first, scriptable, visible in the CMake Tools target dropdown):
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
- `HAL_PCD_Init` runs at boot then sits unused — ~100 µs wasted.
- TinyUSB owns the USB peripheral at runtime. `tud_init(0)` in the `usbDeviceTask` reprograms the registers. The `USB_DRD_FS_IRQHandler` USER CODE block in [Core/Src/stm32c0xx_it.c](Core/Src/stm32c0xx_it.c) hijacks the IRQ via `tud_int_handler(0); return;` — bypasses the HAL handler entirely.
- TinyUSB config: [Core/Inc/tusb_config.h](Core/Inc/tusb_config.h) (CDC ACM, `OPT_MCU_STM32C0`, `OPT_OS_FREERTOS`).
- USB descriptors: [Core/Src/usb_descriptors.c](Core/Src/usb_descriptors.c). VID/PID `0xCafe/0x4001` (TinyUSB example values — replace before any external release).

When touching USB code, the relevant tasks live in [Core/Src/app_freertos.c](Core/Src/app_freertos.c):
- `usbDeviceTask` (priority AboveNormal): `tud_init(0)` then `tud_task()` loop. Blocks on TinyUSB's FreeRTOS queue between events.
- `telemetryTask` (priority Normal): emits `tick N\r\n` once per second when `tud_cdc_connected()`.

## Interrupt priorities

Cortex-M0+ has only 2 priority bits (0..3, lower=higher urgency) and **no `BASEPRI`** — FreeRTOS critical sections mask all interrupts via `PRIMASK`. The X-CUBE-FREERTOS pack enforces `LIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 0`, meaning any ISR with "Uses FreeRTOS functions" checked in CubeMX will be forced to priority 0. Don't fight it — priority differentiation only matters between non-FreeRTOS-aware ISRs.

- All user-facing IRQs (USB, I2C1, I2C2, USART1, EXTI4_15): priority 0, "Uses FreeRTOS functions" ✓
- SysTick / PendSV: priority 3 (FreeRTOS-owned, leave alone)
- TIM2 (FreeRTOS time base): priority 3 (FreeRTOS-owned)

## Code conventions

- All hand-written application code lives in CubeMX **USER CODE BEGIN/END** blocks so regens don't wipe it. The IRQ-handler hijack relies on this — `tud_int_handler(0); return;` is inside `USER CODE BEGIN USB_DRD_FS_IRQn 0` followed by the unreachable HAL call.
- Don't edit files under `cmake/stm32cubemx/` — CubeMX regenerates them. Add custom CMake to the project-level [CMakeLists.txt](CMakeLists.txt) instead.
- TinyUSB is **vendored** (not a submodule) — version bump = re-download `0.x.0.tar.gz` from `github.com/hathach/tinyusb` and re-trim `src/` to the same subset (see [Middlewares/tinyusb/VERSION.txt](Middlewares/tinyusb/VERSION.txt)).

## Host-side tooling

Python + pyserial is the baseline for serial work and automated tests:

```
pip install pyserial
python -m serial.tools.miniterm --list                  # find the COM port
python -m serial.tools.miniterm COM7 115200             # interactive (baud is ignored by CDC)
python scripts/telemetry_check.py [COM7]                # auto-detects if port omitted
```

[scripts/telemetry_check.py](scripts/telemetry_check.py) reads 5 s of telemetry and asserts the `tick N` counter is monotonic. Exit 0 = pass. Foundation for future regression tests (next pass will add a FreeRTOS+CLI command/response harness here).

## Static analysis (clang-tidy)

Configured via [.clang-tidy](.clang-tidy) at the firmware project root. Lints only the files we own or substantially modified (`Core/Src/app_freertos.c`, `Core/Src/usb_descriptors.c`, `Core/Src/stm32c0xx_it.c`) — CubeMX boilerplate, TinyUSB, FreeRTOS, and HAL are excluded by not listing them. Add files to `LINT_FILES` in [scripts/lint.py](scripts/lint.py) as they become eligible.

### Why WSL on Windows

ST's `starm-clang` bundle does **not** include `clang-tidy`, and native Windows clang-tidy (from winget/choco) can't cross-compile arm-none-eabi without the ARM LLVM headers. The local lint workflow mirrors CI exactly by running inside **WSL Ubuntu**: apt-installed `clang-tidy` as the analyzer + ARM LLVM Embedded Toolchain (downloaded to `/opt/llvm-arm`) for the picolibc/arm-none-eabi headers.

### One-time setup

**Windows (via WSL Ubuntu):**
```powershell
scripts\setup-wsl.ps1
```
PowerShell wrapper around `wsl bash scripts/setup-wsl.sh` — handles path/quoting between PowerShell, `wsl`, and bash so callers don't have to. Installs apt packages (`clang-tidy ninja-build cmake python3 curl`) and downloads ARM LLVM Embedded Toolchain 19.1.5 to `/opt/llvm-arm`. ~5 minutes. Prompts for sudo password. Re-runnable, idempotent.

If PowerShell execution policy blocks it:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\setup-wsl.ps1
```

**Linux / macOS:**
```
sudo apt install clang-tidy ninja-build cmake python3 curl    # Debian/Ubuntu
brew install llvm cmake ninja                                  # macOS
# Then download ARM LLVM Embedded Toolchain to /opt/llvm-arm (see setup-wsl.sh
# for the exact recipe — same URL / steps just without sudo apt).
```

### Run lint

```powershell
# Windows (PowerShell wrapper around the WSL invocation):
scripts\run-lint.ps1                       # warnings as warnings
scripts\run-lint.ps1 --warnings-as-errors  # CI mode
scripts\run-lint.ps1 --fix                 # apply auto-fixes; review diff after
```

```
# Linux/macOS (direct):
bash scripts/run-lint.sh [--fix | --warnings-as-errors]
```

[scripts/run-lint.sh](scripts/run-lint.sh) configures the `ci-Debug` preset to emit `compile_commands.json`, extracts ARM clang's implicit include paths via `-v -E`, passes each as `--extra-arg=-isystem<dir>`, then delegates to `scripts/lint.py`. Configure is skipped if `build/ci-Debug/compile_commands.json` is fresher than `CMakeLists.txt`/`CMakePresets.json`.

### Integrations

- **CMake targets:** `cmake --build build/<preset> --target lint` (or `lint-fix`). On Windows the target invokes `wsl bash scripts/run-lint.sh`; on Linux/macOS it runs directly. Shows up in the CMake Tools target dropdown.
- **VS Code tasks:** "Lint (clang-tidy, WSL)" and "Lint (clang-tidy, WSL) --fix" in [.vscode/tasks.json](.vscode/tasks.json). The non-fix task has a problem matcher so warnings land in the Problems panel as clickable references.
- **CI:** the `lint` job in [.github/workflows/firmware-build.yml](../../../.github/workflows/firmware-build.yml) runs essentially the same recipe (apt clang-tidy + extracted ARM include paths) with `--warnings-as-errors`. Fails the build on any warning.
