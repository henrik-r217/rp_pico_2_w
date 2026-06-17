#include <stdio.h>      // printf()
#include <stdint.h>     // uint8_t, uint16_t
#include <stdbool.h>    // bool, true, false

#include "pico/stdlib.h"    // stdio_init_all(), sleep_ms(), GPIO-funktioner
#include "hardware/i2c.h"   // i2c_init(), i2c_read/write_timeout_us()

/*
 * =========================
 * Konfiguration
 * =========================
 *
 * Här väljer vi vilken I2C-port och vilka GPIO-pinnar som används.
 *
 * För Pico / Pico 2 / Pico W / Pico 2 W är ett vanligt exempel:
 *   SDA = GP4
 *   SCL = GP5
 *
 * SHT30 använder normalt I2C-adress 0x44 eller 0x45
 * beroende på hur ADDR-pinnen är kopplad på modulen.
 */
#define I2C_PORT      i2c0
#define I2C_SDA_PIN   4
#define I2C_SCL_PIN   5
#define I2C_BAUDRATE  100000   // 100 kHz är en bra och stabil start

#define SHT30_ADDR    0x44     // Vanligaste adressen. Testa 0x45 om sensorn inte svarar.

/*
 * =========================
 * CRC-8 för SHT30
 * =========================
 *
 * SHT30 skickar 6 byte vid en mätning:
 *
 *   Byte 0: Temp MSB
 *   Byte 1: Temp LSB
 *   Byte 2: CRC för temperaturbytes
 *   Byte 3: RH MSB
 *   Byte 4: RH LSB
 *   Byte 5: CRC för humiditybytes
 *
 * Sensorn använder CRC-8 med:
 *   polynomial = 0x31
 *   initial value = 0xFF
 *
 * Vi använder funktionen för att verifiera att datan inte blivit korrupt.
 */
static uint8_t sht30_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;

    // Iterera över varje databajt
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];

        // För varje bit i byten
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ 0x31);
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

/*
 * =========================
 * Läs en mätning från SHT30
 * =========================
 *
 * Funktionen gör följande:
 *
 * 1. Skickar kommandot 0x2400 till sensorn
 *    - Single shot measurement
 *    - High repeatability
 *    - Clock stretching disabled
 *
 * 2. Väntar en kort stund medan sensorn mäter
 *
 * 3. Läser tillbaka 6 byte
 *
 * 4. Verifierar CRC för temperatur och luftfuktighet
 *
 * 5. Konverterar rådata till:
 *      temperatur i grader C
 *      relativ luftfuktighet i %RH
 *
 * Returnerar:
 *   true  = lyckad läsning
 *   false = fel vid I2C eller CRC
 */
static bool sht30_read(float *temperature_c, float *humidity_rh) {
    /*
     * Kommando 0x2400:
     *   0x24 0x00
     *
     * Detta är ett standardkommando i single-shot-läge
     * för hög noggrannhet utan clock stretching.
     */
    uint8_t cmd[2] = {0x24, 0x00};

    /*
     * Här kommer vi lagra de 6 bytes som sensorn returnerar:
     *   temp_msb, temp_lsb, temp_crc, hum_msb, hum_lsb, hum_crc
     */
    uint8_t data[6];

    /*
     * Skicka mätkommando till sensorn.
     *
     * Parametrar:
     *   I2C_PORT      = vilken I2C-kontroller vi använder
     *   SHT30_ADDR    = slav-adress
     *   cmd           = data att skriva
     *   sizeof(cmd)   = antal byte att skriva (2)
     *   false         = skicka STOP i slutet
     *   10000         = timeout i mikrosekunder
     */
    int ret = i2c_write_timeout_us(
        I2C_PORT,
        SHT30_ADDR,
        cmd,
        sizeof(cmd),
        false,
        10000
    );

    // Om vi inte lyckades skriva exakt 2 bytes så har något gått fel
    if (ret != sizeof(cmd)) {
        printf("SHT30 command write failed, ret=%d\n", ret);
        return false;
    }

    /*
     * Vänta på att mätningen ska bli klar.
     *
     * För high repeatability behövs typiskt runt 15 ms.
     * 20 ms ger lite marginal.
     */
    sleep_ms(20);

    /*
     * Läs tillbaka 6 bytes från sensorn.
     */
    ret = i2c_read_timeout_us(
        I2C_PORT,
        SHT30_ADDR,
        data,
        sizeof(data),
        false,
        10000
    );

    // Vi måste få exakt 6 bytes
    if (ret != sizeof(data)) {
        printf("SHT30 read failed, ret=%d\n", ret);
        return false;
    }

    /*
     * Kontrollera CRC för temperaturdata:
     *   data[0], data[1] ska ge CRC som matchar data[2]
     */
    if (sht30_crc8(&data[0], 2) != data[2]) {
        printf("Temperature CRC error\n");
        return false;
    }

    /*
     * Kontrollera CRC för humiditydata:
     *   data[3], data[4] ska ge CRC som matchar data[5]
     */
    if (sht30_crc8(&data[3], 2) != data[5]) {
        printf("Humidity CRC error\n");
        return false;
    }

    /*
     * Slå ihop två bytes till 16-bitars råvärden.
     *
     * Temperatur:
     *   raw_temp = data[0] << 8 | data[1]
     *
     * Luftfuktighet:
     *   raw_hum = data[3] << 8 | data[4]
     */
    uint16_t raw_temp = ((uint16_t)data[0] << 8) | data[1];
    uint16_t raw_hum  = ((uint16_t)data[3] << 8) | data[4];

    /*
     * Omvandla enligt SHT30-databladets formler:
     *
     * Temperatur (°C):
     *   T = -45 + 175 * raw_temp / 65535
     *
     * Relativ luftfuktighet (%RH):
     *   RH = 100 * raw_hum / 65535
     */
    *temperature_c = -45.0f + 175.0f * ((float)raw_temp / 65535.0f);
    *humidity_rh   = 100.0f * ((float)raw_hum / 65535.0f);

    return true;
}

/*
 * =========================
 * main()
 * =========================
 *
 * Programflöde:
 *
 * 1. Initiera USB-serial så printf kan visas i terminal
 * 2. Initiera I2C
 * 3. Konfigurera SDA/SCL-pinnar till I2C-funktion
 * 4. Aktivera pull-up på SDA/SCL
 * 5. Läs sensorn periodiskt
 * 6. Skriv ut temperatur och RH
 */
int main(void) {
    /*
     * Initierar stdio för USB/UART beroende på projektets config.
     * Med pico_enable_stdio_usb(..., 1) brukar detta gå till USB-serial.
     */
    stdio_init_all();

    /*
     * Vänta lite så att seriell terminal hinner koppla upp sig.
     * Praktiskt när man använder USB och vill se första printf-raden.
     */
    sleep_ms(2000);

    printf("Pico 2 W SHT30 test\n");

    /*
     * Initiera I2C-kontrollern med vald hastighet.
     */
    i2c_init(I2C_PORT, I2C_BAUDRATE);

    /*
     * Sätt valda GPIO-pinnar till I2C-funktion.
     */
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);

    /*
     * Aktivera pull-up internt.
     *
     * OBS:
     * Många I2C-moduler har redan externa pull-ups.
     * I2C kräver pull-up-resistorer för att fungera korrekt.
     */
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    /*
     * Huvudloop:
     * Läs sensorn varannan sekund och skriv ut resultatet.
     */
    while (true) {
        float temperature = 0.0f;
        float humidity = 0.0f;

        if (sht30_read(&temperature, &humidity)) {
            printf("Temperature: %.2f C, Humidity: %.2f %%RH\n",
                   temperature,
                   humidity);
        } else {
            printf("SHT30 read failed\n");
        }

        sleep_ms(2000);
    }
}
