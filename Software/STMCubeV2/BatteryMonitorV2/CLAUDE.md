# BatteryMonitorV2 — Claude conventions

STM32C071 / FreeRTOS 11.2 (X-CUBE-FREERTOS pack, *not* legacy middleware) / TinyUSB-CDC over USB-FS. 24 KB SRAM, 64 KB flash. Single USB-FS port on PA11/PA12 used for both DFU programming and CDC telemetry. See [README.md](README.md) for setup, build, flash, lint, test, and coverage commands.

## Hard rules

1. **Hand-written code lives in CubeMX `USER CODE BEGIN/END` blocks.** Outside those blocks, CubeMX regen will wipe edits.
2. **Don't edit anything under [cmake/stm32cubemx/](cmake/stm32cubemx/)** — CubeMX regenerates it. Add custom CMake to the project-level [CMakeLists.txt](CMakeLists.txt) instead.
3. **TinyUSB owns the USB peripheral at runtime, not HAL.** `MX_USB_PCD_Init` runs at boot and is harmless; `tud_init(0)` reprograms the registers. The IRQ handler in [Core/Src/stm32c0xx_it.c](Core/Src/stm32c0xx_it.c) uses a `USER CODE BEGIN USB_DRD_FS_IRQn 0` block to redirect to `tud_int_handler(0); return;` *before* the HAL call. Don't remove that early return.
4. **TinyUSB is vendored**, not a submodule — see [Middlewares/tinyusb/VERSION.txt](Middlewares/tinyusb/VERSION.txt) for the bump procedure.

## Conventions

- **Interrupt priorities:** CM0+ has no `BASEPRI`; the X-CUBE-FREERTOS pack forces every IRQ with "Uses FreeRTOS functions" checked to priority 0 in CubeMX. Don't fight it. SysTick / PendSV / TIM2 (FreeRTOS-owned) stay at 3.
- **Adding hand-written files:** add to `LINT_FILES` in [scripts/lint.py](scripts/lint.py). For testable modules, the production source path is auto-linked by Ceedling from the matching `test_<module>.c` filename — no extra config needed unless the module pulls in HAL symbols not yet stubbed in [test/support/](test/support/).
- **Active-high I/O wrappers:** `enable` → `GPIO_PIN_SET`, `disable` → `GPIO_PIN_RESET`. If hardware inverts a signal, invert in the wrapper ([Core/Src/direct_io.c](Core/Src/direct_io.c)), never at the call site.
- **Flash via DFU only.** SWD-only flags (`-hardRst`, `-rst`) on `STM32_Programmer_CLI` fail with a DFU connection. After flash completes, the user taps RESET (without holding BOOT0) to exit DFU and run the new firmware.

## Where things live

| Topic | File |
|---|---|
| Hardware pin map | [BatteryMonitorV2.ioc](BatteryMonitorV2.ioc) (CubeMX) + [Core/Inc/main.h](Core/Inc/main.h) for defines |
| GPIO/PWM wrappers | [Core/Src/direct_io.c](Core/Src/direct_io.c) |
| FreeRTOS tasks | [Core/Src/app_freertos.c](Core/Src/app_freertos.c) USER CODE blocks |
| USB descriptors / VID-PID | [Core/Src/usb_descriptors.c](Core/Src/usb_descriptors.c) — currently `0xCafe/0x4001` (TinyUSB example values; swap before external release) |
| TinyUSB config | [Core/Inc/tusb_config.h](Core/Inc/tusb_config.h) |
| Lint config | [.clang-tidy](.clang-tidy) (rule selection) + [scripts/lint.py](scripts/lint.py) (file list) |
| Test config | [test/project.yml](test/project.yml) (Ceedling) |
| CI workflow | [.github/workflows/firmware-build.yml](../../../.github/workflows/firmware-build.yml) |
| Telemetry parser | [scripts/telemetry_check.py](scripts/telemetry_check.py) |

## Build env quick-ref

Bundled tools live under `%CUBE_BUNDLE_PATH%`. Versions drift — discover from the existing build cache:

```bash
grep -E "^CMAKE_(C_COMPILER|MAKE_PROGRAM|COMMAND|TOOLCHAIN_FILE):" build/<Preset>/CMakeCache.txt
```

**Cache gotcha:** when VS Code's CMake Tools extension configures, it writes `CMAKE_COMMAND:UNINITIALIZED=cube-cmake` (a wrapper not on PATH outside the extension). For CLI builds, either do one direct invocation of the bundled `cmake.exe` first or bootstrap by enumerating `$env:CUBE_BUNDLE_PATH`. Full recipes in [README.md](README.md#building-from-outside-vs-code).

Local lint / unit-test / coverage all flow through **WSL Ubuntu** (`scripts/setup-wsl.ps1` for one-time setup). They share the same toolchain CI uses, so local == CI.
