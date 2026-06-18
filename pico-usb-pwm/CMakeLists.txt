cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(pico_usb_pwm C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pico_usb_pwm
    main.c
)

target_link_libraries(pico_usb_pwm
    pico_stdlib
    hardware_pwm
    hardware_clocks
)

# USB CDC serial enabled, UART disabled.
# Pico SDK USB stdio kan aktiveras via pico_enable_stdio_usb(target 1). 
# Detta ger stdio över USB CDC. 
pico_enable_stdio_usb(pico_usb_pwm 1)
pico_enable_stdio_uart(pico_usb_pwm 0)

pico_add_extra_outputs(pico_usb_pwm)
