#ifndef SHT30_H
#define SHT30_H

#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "hardware/i2c.h"

/*
 * Specialvärde för automatisk adressdetektering.
 * Drivern provar då 0x44 först och sedan 0x45.
 */
#define SHT30_ADDR_AUTO 0xFF

/*
 * Standardadresser för SHT30 / SHT3x.
 */
#define SHT30_ADDR_44   0x44
#define SHT30_ADDR_45   0x45

/*
 * Felkoder för tydligare felsökning.
 */
typedef enum {
    SHT30_OK = 0,
    SHT30_ERR_PARAM = -1,
    SHT30_ERR_NOT_INITIALIZED = -2,
    SHT30_ERR_I2C_WRITE = -3,
    SHT30_ERR_I2C_READ = -4,
    SHT30_ERR_TEMP_CRC = -5,
    SHT30_ERR_HUM_CRC = -6,
    SHT30_ERR_NO_DEVICE_FOUND = -7
} sht30_status_t;

/*
 * Device-struktur som håller all information om den aktuella sensorn.
 */
typedef struct {
    i2c_inst_t *i2c;     // t.ex. i2c0 eller i2c1
    uint sda_pin;        // GPIO för SDA
    uint scl_pin;        // GPIO för SCL
    uint baudrate;       // I2C-hastighet, t.ex. 100000
    uint8_t addr;        // I2C-adress: 0x44 eller 0x45
    bool initialized;    // true när init lyckats
} sht30_t;

/*
 * Initiera SHT30-driver och I2C-pinnar.
 *
 * Parametrar:
 *   dev      - pekare till device-struktur
 *   i2c      - i2c0 eller i2c1
 *   sda_pin  - GPIO för SDA
 *   scl_pin  - GPIO för SCL
 *   baudrate - I2C-hastighet, t.ex. 100000
 *   addr     - 0x44, 0x45 eller SHT30_ADDR_AUTO
 *
 * Returnerar:
 *   SHT30_OK om allt lyckades
 *   annars en negativ felkod
 */
int sht30_init(sht30_t *dev,
               i2c_inst_t *i2c,
               uint sda_pin,
               uint scl_pin,
               uint baudrate,
               uint8_t addr);

/*
 * Läs temperatur och luftfuktighet från sensorn.
 *
 * Returnerar:
 *   SHT30_OK om läsningen lyckades
 *   annars en negativ felkod
 */
int sht30_read(sht30_t *dev, float *temperature_c, float *humidity_rh);

/*
 * Returnera en läsbar text för felkoden.
 */
const char *sht30_strerror(int status);

#endif // SHT30_H
