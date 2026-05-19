cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME BatteryMonitorV2)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Application
    Core/Src/direct_io.c

    # TinyUSB device-mode CDC ACM stack
    Core/Src/usb_descriptors.c
    Middlewares/tinyusb/src/tusb.c
    Middlewares/tinyusb/src/common/tusb_fifo.c
    Middlewares/tinyusb/src/device/usbd.c
    Middlewares/tinyusb/src/device/usbd_control.c
    Middlewares/tinyusb/src/class/cdc/cdc_device.c
    Middlewares/tinyusb/src/portable/st/stm32_fsdev/dcd_stm32_fsdev.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Middlewares/tinyusb/src
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# Post-build: emit raw .bin alongside the .elf for DFU upload
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/${CMAKE_PROJECT_NAME}.bin
    COMMENT "Generating ${CMAKE_PROJECT_NAME}.bin"
)

# DFU flash target. Prerequisites: board in DFU mode (hold BOOT0, tap RESET).
# After flash completes, tap RESET (without BOOT0) to exit DFU and run the new firmware.
# Override STM32_PROGRAMMER_CLI via -DSTM32_PROGRAMMER_CLI=<path> if installed elsewhere.
set(STM32_PROGRAMMER_CLI
    "C:/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe"
    CACHE FILEPATH "Path to STM32_Programmer_CLI for DFU flashing"
)
add_custom_target(flash
    COMMAND "${STM32_PROGRAMMER_CLI}" -c port=USB1 -w $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
    DEPENDS ${CMAKE_PROJECT_NAME}
    USES_TERMINAL
    COMMENT "Flashing $<TARGET_FILE:${CMAKE_PROJECT_NAME}> via DFU..."
)

# clang-tidy lint target. Runs scripts/run-lint.sh inside WSL on Windows,
# or directly on Linux/macOS. Requires one-time setup: `wsl bash scripts/setup-wsl.sh`
# (Windows) or installing clang-tidy + the ARM LLVM Embedded Toolchain.
if(CMAKE_HOST_WIN32)
    add_custom_target(lint
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run-lint.ps1
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Running clang-tidy via WSL..."
    )
    add_custom_target(lint-fix
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run-lint.ps1 --fix
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Running clang-tidy --fix via WSL..."
    )
    add_custom_target(test_unit
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run-tests.ps1
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Running host-side unit tests via WSL (Ceedling)..."
    )
    add_custom_target(test_coverage
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run-coverage.ps1
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Running tests with coverage instrumentation via WSL..."
    )
else()
    add_custom_target(lint
        COMMAND bash scripts/run-lint.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Running clang-tidy..."
    )
    add_custom_target(lint-fix
        COMMAND bash scripts/run-lint.sh --fix
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Running clang-tidy --fix..."
    )
    add_custom_target(test_unit
        COMMAND bash scripts/run-tests.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Running host-side unit tests (Ceedling)..."
    )
    add_custom_target(test_coverage
        COMMAND bash scripts/run-coverage.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Running tests with coverage instrumentation..."
    )
endif()
