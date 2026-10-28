#include "sensors/convert.h"

/* ---------------- MLX90614 ---------------- */

uint8_t mlx_pec(const uint8_t *bytes, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

bool mlx_reply_ok(uint8_t addr, uint8_t cmd, const uint8_t reply[3])
{
    /* The address byte on the wire is the 7-bit address plus a read/write
     * bit: 0 for the write that sends the command, 1 for the read. */
    const uint8_t sent[5] = {
        (uint8_t)(addr << 1), cmd, (uint8_t)((addr << 1) | 1), reply[0], reply[1],
    };
    return mlx_pec(sent, sizeof sent) == reply[2];
}

bool mlx_raw_to_mC(uint16_t raw, int32_t *mc)
{
    if (raw & 0x8000)
        return false;          /* error flag: the chip says this reading is bad */
    *mc = (int32_t)raw * 20 - 273150;
    return true;
}

void mlx_fill(struct dsm_record *r, uint16_t obj_raw, uint16_t amb_raw)
{
    int32_t obj = 0, amb = 0;
    bool ok = mlx_raw_to_mC(obj_raw, &obj);
    ok = mlx_raw_to_mC(amb_raw, &amb) && ok;
    r->v[0] = obj;
    r->v[1] = amb;
    if (ok)
        r->flags |= DSM_FLAG_VALID;
}

/* ---------------- TSL2591 ---------------- */

int32_t tsl_gain(uint8_t control)
{
    static const int32_t gain[4] = {1, 25, 428, 9876};
    return gain[(control >> 4) & 0x3];
}

int32_t tsl_integration_ms(uint8_t control)
{
    uint8_t atime = control & 0x7;
    return atime <= 5 ? 100 * (atime + 1) : 0;
}

uint16_t tsl_max_count(uint8_t control)
{
    return (control & 0x7) == 0 ? 37888 : 65535;
}

void tsl_fill(struct dsm_record *r, const uint8_t data[4], uint8_t control)
{
    uint16_t ch0 = (uint16_t)(data[0] | (data[1] << 8));   /* low byte comes first */
    uint16_t ch1 = (uint16_t)(data[2] | (data[3] << 8));
    uint16_t max = tsl_max_count(control);

    r->v[0] = ch0;
    r->v[1] = ch1;
    r->v[2] = tsl_gain(control);
    r->v[3] = tsl_integration_ms(control);

    /* A channel at full scale only says "at least this bright", so it
     * can't be trusted as a measurement. */
    if (ch0 < max && ch1 < max && r->v[3] != 0)
        r->flags |= DSM_FLAG_VALID;
}

/* ---------------- BME280 ---------------- */

static uint16_t u16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static int16_t  s16(const uint8_t *b) { return (int16_t)u16(b); }

void bme280_parse_calib(struct bme280_calib *c, const uint8_t lo[26], const uint8_t hi[7])
{
    c->T1 = u16(lo + 0);
    c->T2 = s16(lo + 2);
    c->T3 = s16(lo + 4);
    c->P1 = u16(lo + 6);
    c->P2 = s16(lo + 8);
    c->P3 = s16(lo + 10);
    c->P4 = s16(lo + 12);
    c->P5 = s16(lo + 14);
    c->P6 = s16(lo + 16);
    c->P7 = s16(lo + 18);
    c->P8 = s16(lo + 20);
    c->P9 = s16(lo + 22);
    c->H1 = lo[25];            /* 0xA1; lo[24] is 0xA0, not calibration */

    c->H2 = s16(hi + 0);
    c->H3 = hi[2];
    /* H4 = 0xE4 (top 8 bits) and the low half of 0xE5. H5 = the high half
     * of 0xE5 and 0xE6 (top 8 bits). The top byte is read as signed so a
     * negative 12-bit value comes out negative. */
    c->H4 = (int16_t)((int8_t)hi[3] * 16 | (hi[4] & 0x0F));
    c->H5 = (int16_t)((int8_t)hi[5] * 16 | (hi[4] >> 4));
    c->H6 = (int8_t)hi[6];
}

void bme280_parse_data(const uint8_t d[8], int32_t *adc_p, int32_t *adc_t, int32_t *adc_h)
{
    *adc_p = (int32_t)(((uint32_t)d[0] << 12) | ((uint32_t)d[1] << 4) | (d[2] >> 4));
    *adc_t = (int32_t)(((uint32_t)d[3] << 12) | ((uint32_t)d[4] << 4) | (d[5] >> 4));
    *adc_h = (int32_t)(((uint32_t)d[6] << 8) | d[7]);
}

/* The next three are the datasheet's code (4.2.3) with one change: in C,
 * shifting a negative number left is undefined behavior, so every "<< n"
 * on a value that can be negative is written as "* 2^n". Right shifts of
 * negative numbers are kept: gcc and clang shift in the sign bit, which is
 * what the datasheet assumes. */

int32_t bme280_temp(const struct bme280_calib *c, int32_t adc_t, int32_t *t_fine)
{
    int32_t var1 = (((adc_t >> 3) - ((int32_t)c->T1 << 1)) * (int32_t)c->T2) >> 11;
    int32_t var2 = (((((adc_t >> 4) - (int32_t)c->T1) * ((adc_t >> 4) - (int32_t)c->T1)) >> 12)
                    * (int32_t)c->T3) >> 14;
    *t_fine = var1 + var2;
    return (*t_fine * 5 + 128) >> 8;
}

uint32_t bme280_press(const struct bme280_calib *c, int32_t adc_p, int32_t t_fine)
{
    int64_t var1 = (int64_t)t_fine - 128000;
    int64_t var2 = var1 * var1 * (int64_t)c->P6;
    var2 = var2 + var1 * (int64_t)c->P5 * 131072;                  /* << 17 */
    var2 = var2 + (int64_t)c->P4 * 34359738368;                    /* << 35 */
    var1 = ((var1 * var1 * (int64_t)c->P3) >> 8) + var1 * (int64_t)c->P2 * 4096;   /* << 12 */
    var1 = (((int64_t)1 << 47) + var1) * (int64_t)c->P1 >> 33;
    if (var1 == 0)
        return 0;              /* avoids dividing by zero */
    int64_t p = 1048576 - adc_p;
    p = ((p * 2147483648) - var2) * 3125 / var1;                   /* << 31 */
    var1 = ((int64_t)c->P9 * (p >> 13) * (p >> 13)) >> 25;
    var2 = ((int64_t)c->P8 * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (int64_t)c->P7 * 16;            /* << 4 */
    return (uint32_t)p;
}

uint32_t bme280_hum(const struct bme280_calib *c, int32_t adc_h, int32_t t_fine)
{
    int32_t x = t_fine - 76800;
    int32_t v = (((((adc_h << 14) - (int32_t)c->H4 * 1048576 - (int32_t)c->H5 * x)   /* << 20 */
                   + 16384) >> 15)
                 * (((((((x * (int32_t)c->H6) >> 10) * (((x * (int32_t)c->H3) >> 11) + 32768))
                       >> 10) + 2097152) * (int32_t)c->H2 + 8192) >> 14));
    v = v - (((((v >> 15) * (v >> 15)) >> 7) * (int32_t)c->H1) >> 4);
    v = v < 0 ? 0 : v;
    v = v > 419430400 ? 419430400 : v;
    return (uint32_t)(v >> 12);
}

void bme280_fill(struct dsm_record *r, const struct bme280_calib *c, const uint8_t d[8])
{
    int32_t adc_p, adc_t, adc_h, t_fine;
    bme280_parse_data(d, &adc_p, &adc_t, &adc_h);

    /* Still at the reset value: that measurement never ran. */
    if (adc_t == 0x80000 || adc_p == 0x80000 || adc_h == 0x8000)
        return;

    int32_t t = bme280_temp(c, adc_t, &t_fine);
    uint32_t p = bme280_press(c, adc_p, t_fine);
    uint32_t h = bme280_hum(c, adc_h, t_fine);

    r->v[0] = t * 10;                                 /* 0.01 °C -> m°C */
    r->v[1] = (int32_t)((h * 1000u + 512u) / 1024u);  /* Q22.10 %RH -> milli-%RH, rounded */
    r->v[2] = (int32_t)((p + 128u) / 256u);           /* Q24.8 Pa -> Pa, rounded */
    r->flags |= DSM_FLAG_VALID;
}
