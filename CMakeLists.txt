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
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(piicomouse2 C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(piicomouse2 main.c wiimote.c hid.c usb_descriptors.c)

pico_set_program_name(piicomouse2 "piicomouse2")
pico_set_program_version(piicomouse2 "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(piicomouse2 1)
pico_enable_stdio_usb(piicomouse2 1)

# Add the standard library to the build
target_link_libraries(piicomouse2
    pico_stdlib
    tinyusb_device
    tinyusb_board
    pico_cyw43_arch_none
    pico_btstack_classic
    pico_btstack_cyw43)

target_compile_definitions(piicomouse2 PRIVATE
    PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=0
    PICO_STDIO_USB_ENABLE_TINYUSB_INIT=1
    PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=1
    PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=1
    PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=1
    PICO_STDIO_USB_RESET_INTERFACE_SUPPORT_RESET_TO_BOOTSEL=1
    PICO_STDIO_USB_RESET_INTERFACE_SUPPORT_RESET_TO_FLASH_BOOT=1
    PICO_STDIO_USB_RESET_INTERFACE_SUPPORT_MS_OS_20_DESCRIPTOR=1
    PICO_STDIO_USB_RESET_MAGIC_BAUD_RATE=1200
    PICO_STDIO_USB_RESET_BOOTSEL_INTERFACE_DISABLE_MASK=0
    PICO_STDIO_USB_RESET_RESET_TO_FLASH_DELAY_MS=100
)

# Add the standard include files to the build
target_include_directories(piicomouse2 PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

# Add any user requested libraries
target_link_libraries(piicomouse2 
        
        )

pico_add_extra_outputs(piicomouse2)

