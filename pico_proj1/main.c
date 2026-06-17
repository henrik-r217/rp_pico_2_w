#include <stdio.h>

#include "pico/stdlib.h"
#include "sht30.h"

/*
 * Anpassa dessa efter din koppling.
 *
 * Exempel:
 *   SDA -> GP4
 *   SCL -> GP5
 *
 * På Pico 2 W är detta ett vanligt och bra val.
 */
#define APP_I2C_PORT     i2c0
#define APP_I2C_SDA_PIN  4
#define APP_I2C_SCL_PIN  5
#define APP_I2C_BAUDRATE 100000

int main(void) {
    /*
     * Initiera USB/UART-stdio så att printf fungerar i terminalen.
     */
    stdio_init_all();

    /*
     * Vänta lite så att USB-seriell anslutning hinner komma upp.
     */
    sleep_ms(2000);

    printf("SHT30 example using Raspberry Pi Pico SDK\n");

    /*
     * Skapa driver-instans.
     */
    sht30_t sensor;

    /*
     * Initiera sensorn.
     *
     * Här använder vi autodetektering av adress:
     *   0x44 eller 0x45
     */
    int status = sht30_init(&sensor,
                            APP_I2C_PORT,
                            APP_I2C_SDA_PIN,
                            APP_I2C_SCL_PIN,
                            APP_I2C_BAUDRATE,
                            SHT30_ADDR_AUTO);

    if (status != SHT30_OK) {
        printf("sht30_init failed: %s\n", sht30_strerror(status));

        /*
         * Om init misslyckas stannar vi här så felet blir tydligt.
         */
        while (true) {
            sleep_ms(1000);
        }
    }

    printf("SHT30 initialized successfully at address 0x%02X\n", sensor.addr);

    while (true) {
        float temperature_c = 0.0f;
        float humidity_rh   = 0.0f;

        status = sht30_read(&sensor, &temperature_c, &humidity_rh);
        if (status == SHT30_OK) {
            printf("Temperature: %.2f C, Humidity: %.2f %%RH\n",
                   temperature_c,
                   humidity_rh);
        } else {
            printf("sht30_read failed: %s\n", sht30_strerror(status));
        }

        sleep_ms(2000);
    }
}
