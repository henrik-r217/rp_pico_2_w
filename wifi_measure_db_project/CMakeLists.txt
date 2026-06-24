cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pico2_w)

include(pico_sdk_import.cmake)


project(pico_http_post C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()




add_executable(pico_http_post
      main.c
      sht30.c 
)

target_include_directories(pico_http_post PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(pico_http_post
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
    hardware_i2c
    hardware_clocks
)

    
target_compile_definitions(pico_http_post PRIVATE
    WIFI_SSID=\"${WIFI_SSID}\"
    WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
    DEBUG=\"${DEBUG}\"
    API_KEY=\"${API_KEY}\"
)



pico_enable_stdio_usb(pico_http_post 1)
pico_enable_stdio_uart(pico_http_post 0)

pico_add_extra_outputs(pico_http_post)


