#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico_sensor_lib.h"

static bool i2c_bus_active = false;
static i2c_inst_t * i2c_bus = NULL;
static int i2c_temp_sensors = 0;


int main() {

    int res;
    // Initialize sensor
    void *ctx = NULL;
    stdio_init_all();
    sleep_ms(2000);
    setup_default_uart();
    sleep_ms(5000);
    printf("snart init\n");
    res = i2c_init_sensor(get_i2c_sensor_type("SHT3x"), i2c_bus, 0x44, &ctx);
    if (res)
        {
            printf("failed to initialize sensor...\n");
        }      
    
    int delay = i2c_start_measurement(ctx);
    if (delay < 0)
        {
            printf("failed to initiate measurement...\n");
        }


    float temp, pressure, humidity;
    res = i2c_read_measurement(ctx, &temp, &pressure, &humidity);
    if (res)
        {
            printf("failed to read measurements...\n");
        }
    printf("sht30_0: temperature is %0.2fC.\n", temp);
    printf("sht30_0: humidity is %0.2f%%.\n", humidity);
    
 

    sleep_ms(2000);
    printf("Hello, world!\n");
    sleep_ms(2000);
    printf("Ny rad och ett nytt medelande.\n");
    
        


    return 0;
}
