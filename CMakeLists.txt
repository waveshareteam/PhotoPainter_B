# == DO NEVER EDIT THE NEXT LINES for Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.0.0)
set(toolchainVersion 13_2_Rel1)
set(picotoolVersion 2.0.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")
cmake_minimum_required(VERSION 3.12)
set(PICO_SDK_FETCH_FROM_GIT on)
include(pico_sdk_import.cmake)
project(Pico_ePaper_73)
pico_sdk_init()

# add a compilation subdirectory
add_subdirectory(lib/Config)
add_subdirectory(lib/e-Paper)
add_subdirectory(lib/RTC)
add_subdirectory(lib/Fonts)
add_subdirectory(lib/tjpgd)
add_subdirectory(lib/GUI)
add_subdirectory(lib/led) 
add_subdirectory(examples)
add_subdirectory(lib/FatFs_SPI build)

# add a header directory
include_directories(examples)
include_directories(./lib/Config)
include_directories(./lib/GUI)
include_directories(./lib/RTC)
include_directories(./lib/led)

# generate an executable file
add_executable(epd
main.c
usb_msc/usb_disk.c
usb_msc/usb_descriptors.c
)

# USB disk mode: the app provides its own TinyUSB config (CDC + MSC) and
# descriptors, while keeping pico_stdio_usb's usual behaviour in normal mode.
target_include_directories(epd PRIVATE ${CMAKE_CURRENT_LIST_DIR}/usb_msc)
target_compile_definitions(epd PRIVATE
    PICO_STDIO_USB_ENABLE_TINYUSB_INIT=1
    PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=1
    PICO_ENABLE_USB_RESET_VIA_BAUD_RATE=1
    PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE=0
)

# enable usb output, disable uart output
pico_enable_stdio_usb(epd 1)
pico_enable_stdio_uart(epd 0)

# create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(epd)
 
target_link_libraries(epd examples ePaper GUI led Fonts Config RTC FatFs_SPI tinyusb_device hardware_watchdog)