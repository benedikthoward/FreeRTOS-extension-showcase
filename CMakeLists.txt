cmake_minimum_required(VERSION 3.13)

# Set board before SDK init
set(PICO_BOARD pico2_w)

# Pull in SDK (must be before project)
include(pico_sdk_import.cmake)

# Pull in FreeRTOS Kernel
include(FreeRTOS_Kernel_import.cmake)

project(freertos_extension_showcase C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ─── Build options (controlled via presets or -D flags) ────────────────────
option(BUILD_EDF "Build the EDF demo"  OFF)
option(BUILD_SRP "Build the SRP demo"  OFF)
option(BUILD_CBS "Build the CBS demo"  OFF)
option(BUILD_MP  "Build the MP demo"   OFF)

# ─── Forward extension toggles as compile definitions ─────────────────────
# These are set by presets (e.g. -DconfigUSE_EDF=1) and picked up by
# FreeRTOSConfig.h via #ifndef guards.
foreach(_ext EDF SRP CBS MP)
    if(DEFINED configUSE_${_ext})
        add_compile_definitions(configUSE_${_ext}=${configUSE_${_ext}})
    endif()
endforeach()

# Forward multiprocessor scheduling mode flags
foreach(_flag GLOBAL_EDF_ENABLE PARTITIONED_EDF_ENABLE)
    if(DEFINED ${_flag})
        add_compile_definitions(${_flag}=${${_flag}})
    endif()
endforeach()

# ─── FreeRTOS config library ──────────────────────────────────────────────
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config SYSTEM INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/include
)

# ─── Common test harness ──────────────────────────────────────────────────
add_library(common INTERFACE)
target_sources(common INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/common/test_harness.c
    ${CMAKE_CURRENT_LIST_DIR}/common/trace_protocol.c
    ${CMAKE_CURRENT_LIST_DIR}/common/usb_task.c
)
target_include_directories(common INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/common
)

# ─── Helper: wire up a demo target ───────────────────────────────────────
function(add_demo_target NAME)
    add_executable(${NAME} src/${NAME}.c)
    target_include_directories(${NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
    target_link_libraries(${NAME}
        pico_stdlib
        pico_cyw43_arch_none
        FreeRTOS-Kernel
        FreeRTOS-Kernel-Heap4
        common
    )
    # Disable SDK's alarm-based tud_task() poller — our vStartUsbTask()
    # drives tud_task() from a FreeRTOS task instead.
    # Forward DEMO_SCENARIO so each binary runs exactly one scenario.
    if(NOT DEFINED DEMO_SCENARIO)
        set(DEMO_SCENARIO 1)
    endif()
    target_compile_definitions(${NAME} PRIVATE
        PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=0
        DEMO_SCENARIO=${DEMO_SCENARIO}
    )
    pico_enable_stdio_usb(${NAME} 1)
    pico_enable_stdio_uart(${NAME} 0)
    pico_add_extra_outputs(${NAME})
endfunction()

# ─── Blink test (no FreeRTOS, just Pico SDK) ─────────────────────────────
add_executable(blink_test src/blink_test.c)
target_link_libraries(blink_test pico_stdlib pico_cyw43_arch_none)
pico_enable_stdio_usb(blink_test 1)
pico_enable_stdio_uart(blink_test 0)
pico_add_extra_outputs(blink_test)

# ─── Vanilla FreeRTOS blink (no EDF extensions) ──────────────────────────
add_executable(freertos_blink src/freertos_blink.c)
target_include_directories(freertos_blink PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(freertos_blink
    pico_stdlib
    pico_cyw43_arch_none
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
    common
)
target_compile_definitions(freertos_blink PRIVATE
    PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=0
)
pico_enable_stdio_usb(freertos_blink 1)
pico_enable_stdio_uart(freertos_blink 0)
pico_add_extra_outputs(freertos_blink)

# ─── Demo targets ────────────────────────────────────────────────────────
if(BUILD_EDF)
    add_demo_target(edf_demo)
endif()

if(BUILD_SRP)
    add_demo_target(srp_demo)
endif()

if(BUILD_CBS)
    add_demo_target(cbs_demo)
endif()

if(BUILD_MP)
    add_demo_target(mp_demo)
endif()
