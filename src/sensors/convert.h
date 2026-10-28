#ifndef DSM_SENSORS_CONVERT_H
#define DSM_SENSORS_CONVERT_H

#include "common/record.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Turning the bytes each chip sends into the numbers we store. Pure math:
 * no I/O, so all of it is tested on a Mac. The I2C code that fetches the
 * bytes lives in i2c.c and only builds on Linux.
 *
 * Every register number and formula here was checked against the
 * datasheet PDFs on 2026-10-28 (sections named next to each one). */

/* ---- MLX90614: sky (object) and sensor (ambient) IR temperature ----
 * Datasheet: Melexis MLX90614, rev 021. */

#define MLX_ADDR      0x5A   /* default SMBus address */
#define MLX_RAM_TA    0x06   /* ambient (the chip itself), table 14 */
#define MLX_RAM_TOBJ1 0x07   /* object (the sky), table 14 */

/* SMBus PEC: CRC-8, polynomial x^8 + x^2 + x + 1 (0x07), starting at 0,
 * over every byte of the transfer (4.1.4.3.1). */
uint8_t mlx_pec(const uint8_t *bytes, size_t len);

/* A read-word reply is 3 bytes: LSB, MSB, PEC. The PEC covers the address
 * (write), the command, the address (read), LSB and MSB (figure 6). */
bool mlx_reply_ok(uint8_t addr, uint8_t cmd, const uint8_t reply[3]);

/* A RAM temperature word in milli-degrees C: 1 LSB = 0.02 K = 20 m°C, so
 * m°C = raw * 20 - 273150, with no rounding (4.1.8). Returns false, and
 * leaves *mc alone, if the top bit (the error flag) is set. */
bool mlx_raw_to_mC(uint16_t raw, int32_t *mc);

/* SAMPLE payload: v[0] = sky m°C, v[1] = sensor m°C. VALID only if neither
 * word has its error flag set. */
void mlx_fill(struct dsm_record *r, uint16_t obj_raw, uint16_t amb_raw);

/* ---- TSL2591: sky brightness ----
 * Datasheet: ams TSL2591, Apr 2013. */

#define TSL_ADDR      0x29
#define TSL_CMD       0xA0   /* CMD bit 7 + TRANSACTION 01 (normal): OR in the register */
#define TSL_ENABLE    0x00
#define TSL_CONTROL   0x01
#define TSL_ID        0x12
#define TSL_STATUS    0x13
#define TSL_C0DATAL   0x14   /* CH0 = visible + IR; then C0DATAH, C1DATAL, C1DATAH (IR) */
#define TSL_ID_VALUE  0x50
#define TSL_PON       0x01   /* ENABLE bit 0: power on */
#define TSL_AEN       0x02   /* ENABLE bit 1: light sensing on */
#define TSL_AVALID    0x01   /* STATUS bit 0: a full integration has finished */

/* CONTROL fields: AGAIN = bits 5:4, ATIME = bits 2:0. */
int32_t tsl_gain(uint8_t control);            /* 1, 25, 428 or 9876 (typical) */
int32_t tsl_integration_ms(uint8_t control);  /* 100..600; 0 if ATIME is reserved */
uint16_t tsl_max_count(uint8_t control);      /* 37888 at 100 ms, else 65535 */

/* data = the 4 bytes read from C0DATAL on: CH0 low, high, CH1 low, high.
 * SAMPLE payload: v[0] = CH0, v[1] = CH1, v[2] = gain, v[3] = ms.
 * VALID unless a channel reached full scale (saturated) or ATIME is
 * reserved. */
void tsl_fill(struct dsm_record *r, const uint8_t data[4], uint8_t control);

/* ---- BME280: air temperature, humidity, pressure ----
 * Datasheet: Bosch BST-BME280-DS001 rev 1.24. */

#define BME_ADDR      0x76   /* SDO tied to GND */
#define BME_ID        0xD0
#define BME_ID_VALUE  0x60   /* 0x58 would be a BMP280, which has no humidity */
#define BME_CALIB_LO  0x88   /* 26 bytes, 0x88..0xA1 */
#define BME_CALIB_HI  0xE1   /* 7 bytes, 0xE1..0xE7 */
#define BME_CTRL_HUM  0xF2   /* only takes effect after a write to ctrl_meas */
#define BME_STATUS    0xF3
#define BME_CTRL_MEAS 0xF4
#define BME_DATA      0xF7   /* 8 bytes: pressure (3), temperature (3), humidity (2) */

/* The trimming values burned into each chip (table 16). */
struct bme280_calib {
    uint16_t T1;
    int16_t  T2, T3;
    uint16_t P1;
    int16_t  P2, P3, P4, P5, P6, P7, P8, P9;
    uint8_t  H1;
    int16_t  H2;
    uint8_t  H3;
    int16_t  H4, H5;   /* 12-bit values split across 0xE4..0xE6 */
    int8_t   H6;
};

void bme280_parse_calib(struct bme280_calib *c, const uint8_t lo[26], const uint8_t hi[7]);

/* The raw readings from the 8 data bytes: pressure and temperature are
 * 20 bits (msb, lsb, top 4 bits of xlsb), humidity is 16. */
void bme280_parse_data(const uint8_t d[8], int32_t *adc_p, int32_t *adc_t, int32_t *adc_h);

/* The datasheet's integer compensation (4.2.3). Temperature comes first:
 * it also produces t_fine, which pressure and humidity need.
 * Temperature in 0.01 °C; pressure in Pa as Q24.8; humidity in %RH as Q22.10. */
int32_t bme280_temp(const struct bme280_calib *c, int32_t adc_t, int32_t *t_fine);
uint32_t bme280_press(const struct bme280_calib *c, int32_t adc_p, int32_t t_fine);
uint32_t bme280_hum(const struct bme280_calib *c, int32_t adc_h, int32_t t_fine);

/* SAMPLE payload: v[0] = m°C, v[1] = milli-%RH, v[2] = Pa. VALID unless a
 * reading still holds its reset value (0x80000, or 0x8000 for humidity),
 * which means that measurement never ran. */
void bme280_fill(struct dsm_record *r, const struct bme280_calib *c, const uint8_t d[8]);

#endif
