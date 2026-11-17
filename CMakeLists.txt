# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(cdc_qmi_test C CXX ASM)

# TinyUSB CDC(に限らず全USBスタック)のバックグラウンド割り込みを無効にする 
# ただし、USBハードウェア割り込み自体は無効化しない
# ここでの定義は pico-sdk ビルド中に効く。
set(PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK 0 CACHE BOOL
    "Disable stdio USB background task")

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(cdc_qmi_test cdc_qmi_test.c
                cmd.c memprt.c support.c cdc.c
                qmi_floppy.c
                qmi_flash.c qmi_flash_async.c)

# stdio_usb.h 内でも参照されるので、pico-sdkビルド時だけでなく、
# cdc_qmi_testバイナリビルドの際にも本マクロ定義が必要。
target_compile_definitions(cdc_qmi_test PRIVATE
    PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=0
)
pico_set_program_name(cdc_qmi_test "cdc_qmi_test")
pico_set_program_version(cdc_qmi_test "0.1")

# copy_to_ram means bootloader copies binaries to RAM and execute it
pico_set_binary_type(cdc_qmi_test copy_to_ram)

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(cdc_qmi_test 1)
pico_enable_stdio_usb(cdc_qmi_test 1)

# Add the standard library to the build
target_link_libraries(cdc_qmi_test
        pico_stdlib
        hardware_uart
        hardware_clocks
        hardware_flash
        pico_multicore)

# Add the standard include files to the build
target_include_directories(cdc_qmi_test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(cdc_qmi_test)

