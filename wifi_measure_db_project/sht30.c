#include "sht30.h"

#include <stdio.h>

/*
 * =========================
 * Interna konstanter
 * =========================
 */

/*
 * SHT3x single-shot, high repeatability, no clock stretching.
 * Detta är ett vanligt standardkommando för enkel polling-läsning.
 */
#define SHT30_CMD_MEASURE_HIGHREP_MSB 0x24
#define SHT30_CMD_MEASURE_HIGHREP_LSB 0x00

/*
 * Tiden sensorn behöver för att slutföra en mätning i detta läge.
 * 20 ms ger lite säkerhetsmarginal.
 */
#define SHT30_MEASUREMENT_DELAY_MS 20

/*
 * Timeout för I2C-operationer i mikrosekunder.
 */
#define SHT30_I2C_TIMEOUT_US 10000

/*
 * =========================
 * Interna hjälpfunktioner
 * =========================
 */

/*
 * CRC-8 enligt SHT3x-protokollet:
 *   polynomial = 0x31
 *   init value = 0xFF
 *
 * Sensorn skickar CRC för temperatur och humidity separat.
 */
static uint8_t sht30_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];

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
 * Skicka ett single-shot-kommandot och läs tillbaka rådata.
 *
 * data[0..5] fylls med:
 *   temp_msb, temp_lsb, temp_crc, hum_msb, hum_lsb, hum_crc
 */
static int sht30_read_raw_from_addr(sht30_t *dev, uint8_t addr, uint8_t data[6]) {
    if (dev == NULL || data == NULL) {
        return SHT30_ERR_PARAM;
    }

    uint8_t cmd[2] = {
        SHT30_CMD_MEASURE_HIGHREP_MSB,
        SHT30_CMD_MEASURE_HIGHREP_LSB
    };

    /*
     * Starta en mätning.
     */
    int ret = i2c_write_timeout_us(
        dev->i2c,
        addr,
        cmd,
        sizeof(cmd),
        false,
        SHT30_I2C_TIMEOUT_US
    );

    if (ret != (int)sizeof(cmd)) {
        return SHT30_ERR_I2C_WRITE;
    }

    /*
     * Vänta tills sensorn har hunnit mäta klart.
     */
    sleep_ms(SHT30_MEASUREMENT_DELAY_MS);

    /*
     * Läs tillbaka 6 byte resultat.
     */
    ret = i2c_read_timeout_us(
        dev->i2c,
        addr,
        data,
        6,
        false,
        SHT30_I2C_TIMEOUT_US
    );

    if (ret != 6) {
        return SHT30_ERR_I2C_READ;
    }

    /*
     * Verifiera CRC för temperaturdelen.
     */
    if (sht30_crc8(&data[0], 2) != data[2]) {
        return SHT30_ERR_TEMP_CRC;
    }

    /*
     * Verifiera CRC för humiditydelen.
     */
    if (sht30_crc8(&data[3], 2) != data[5]) {
        return SHT30_ERR_HUM_CRC;
    }

    return SHT30_OK;
}

/*
 * Försök hitta sensorn på 0x44 eller 0x45 genom att göra en riktig mätning.
 * Detta är mer robust än att bara "ping:a" adressen.
 */
static int sht30_detect_address(sht30_t *dev, uint8_t *detected_addr) {
    if (dev == NULL || detected_addr == NULL) {
        return SHT30_ERR_PARAM;
    }

    uint8_t data[6];
    int status;

    status = sht30_read_raw_from_addr(dev, SHT30_ADDR_44, data);
    if (status == SHT30_OK) {
        *detected_addr = SHT30_ADDR_44;
        return SHT30_OK;
    }

    status = sht30_read_raw_from_addr(dev, SHT30_ADDR_45, data);
    if (status == SHT30_OK) {
        *detected_addr = SHT30_ADDR_45;
        return SHT30_OK;
    }

    return SHT30_ERR_NO_DEVICE_FOUND;
}

/*
 * =========================
 * Publika funktioner
 * =========================
 */

int sht30_init(sht30_t *dev,
               i2c_inst_t *i2c,
               uint sda_pin,
               uint scl_pin,
               uint baudrate,
               uint8_t addr) {
    if (dev == NULL || i2c == NULL) {
        return SHT30_ERR_PARAM;
    }

    /*
     * Nollställ strukturen först.
     */
    dev->i2c = i2c;
    dev->sda_pin = sda_pin;
    dev->scl_pin = scl_pin;
    dev->baudrate = baudrate;
    dev->addr = 0;
    dev->initialized = false;

    /*
     * Initiera vald I2C-kontroller.
     */
    i2c_init(dev->i2c, dev->baudrate);

    /*
     * Koppla GPIO-pinnarna till I2C-funktionen.
     */
    gpio_set_function(dev->sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(dev->scl_pin, GPIO_FUNC_I2C);

    /*
     * Aktivera pull-up. Många moduler har externa pull-ups,
     * men det skadar normalt inte att även slå på de interna.
     */
    gpio_pull_up(dev->sda_pin);
    gpio_pull_up(dev->scl_pin);

    /*
     * Adresshantering:
     * - Om användaren vill autodetektera: prova 0x44 och 0x45
     * - Annars använd den explicit angivna adressen
     */
    if (addr == SHT30_ADDR_AUTO) {
        int status = sht30_detect_address(dev, &dev->addr);
        if (status != SHT30_OK) {
            return status;
        }
    } else if (addr == SHT30_ADDR_44 || addr == SHT30_ADDR_45) {
        dev->addr = addr;

        /*
         * Verifiera att sensorn verkligen svarar på adressen genom att läsa en mätning.
         */
        uint8_t data[6];
        int status = sht30_read_raw_from_addr(dev, dev->addr, data);
        if (status != SHT30_OK) {
            return status;
        }
    } else {
        return SHT30_ERR_PARAM;
    }

    dev->initialized = true;
    return SHT30_OK;
}

int sht30_read(sht30_t *dev, float *temperature_c, float *humidity_rh) {
    if (dev == NULL || temperature_c == NULL || humidity_rh == NULL) {
        return SHT30_ERR_PARAM;
    }

    if (!dev->initialized) {
        return SHT30_ERR_NOT_INITIALIZED;
    }

    uint8_t data[6];
    int status = sht30_read_raw_from_addr(dev, dev->addr, data);
    if (status != SHT30_OK) {
        return status;
    }

    /*
     * Slå ihop två byte till 16-bitars råvärden.
     */
    uint16_t raw_temp = ((uint16_t)data[0] << 8) | data[1];
    uint16_t raw_hum  = ((uint16_t)data[3] << 8) | data[4];

    /*
     * Omvandling enligt SHT3x-databladets standardformler.
     */
    *temperature_c = -45.0f + 175.0f * ((float)raw_temp / 65535.0f);
    *humidity_rh   = 100.0f * ((float)raw_hum / 65535.0f);

    return SHT30_OK;
}

const char *sht30_strerror(int status) {
    switch (status) {
        case SHT30_OK:
            return "OK";

        case SHT30_ERR_PARAM:
            return "Invalid parameter";

        case SHT30_ERR_NOT_INITIALIZED:
            return "Device not initialized";

        case SHT30_ERR_I2C_WRITE:
            return "I2C write failed";

        case SHT30_ERR_I2C_READ:
            return "I2C read failed";

        case SHT30_ERR_TEMP_CRC:
            return "Temperature CRC error";

        case SHT30_ERR_HUM_CRC:
            return "Humidity CRC error";

        case SHT30_ERR_NO_DEVICE_FOUND:
            return "No SHT30 found at 0x44 or 0x45";

        default:
            return "Unknown error";
    }
}
