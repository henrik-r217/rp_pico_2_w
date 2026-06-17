#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"

#define I2C_PORT i2c0
#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5
#define I2C_BAUDRATE 100000

static bool reserved_addr(uint8_t addr) {
    return (addr & 0x78) == 0 || (addr & 0x78) == 0x78;
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    printf("SHT30 I2C scan start\n");

    i2c_init(I2C_PORT, I2C_BAUDRATE);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    while (true) {
        printf("Scanning I2C bus...\n");

        for (uint8_t addr = 40; addr < 70; addr++) {
            if (reserved_addr(addr)) {
                continue;
            }

            uint8_t dummy;
            int ret = i2c_read_timeout_us(
                I2C_PORT,
                addr,
                &dummy,
                1,
                false,
                1000
            );

            if (ret >= 0) {
                printf("Found device at 0x%02X\n", addr);
            }
        }

        sleep_ms(3000);
    }
}
