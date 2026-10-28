#include "check.h"
#include "suites.h"
#include "sensors/convert.h"

#include <string.h>

static double absd(double x) { return x < 0 ? -x : x; }

/* ---------------- MLX90614 ---------------- */

/* The datasheet's own examples (figures 6-8). The default address 0x5A
 * makes the write address byte 0xB4 and the read address byte 0xB5.
 * Figure 7's caption says "write EEPROM = 0x02", but the byte on the wire
 * is 0x22: EEPROM commands are 001x xxxx (table 15), as the figure shows. */
static void test_mlx_pec_datasheet_examples(void)
{
    const uint8_t read_word[]  = {0xB4, 0x07, 0xB5, 0xD2, 0x3A};
    const uint8_t write_word[] = {0xB4, 0x22, 0x07, 0xC8};
    const uint8_t sleep_cmd[]  = {0xB4, 0xFF};
    uint8_t got;

    got = mlx_pec(read_word, sizeof read_word);
    CHECK(got == 0x30, "read word PEC 0x%02X, want 0x30", got);
    got = mlx_pec(write_word, sizeof write_word);
    CHECK(got == 0x48, "write word PEC 0x%02X, want 0x48", got);
    got = mlx_pec(sleep_cmd, sizeof sleep_cmd);
    CHECK(got == 0xE8, "sleep command PEC 0x%02X, want 0xE8", got);
}

static void test_mlx_reply_check(void)
{
    uint8_t reply[3] = {0xD2, 0x3A, 0x30};   /* figure 6: object temp, 0x3AD2 */
    CHECK(mlx_reply_ok(MLX_ADDR, MLX_RAM_TOBJ1, reply), "the datasheet's reply was rejected");

    reply[0] ^= 0x01;
    CHECK(!mlx_reply_ok(MLX_ADDR, MLX_RAM_TOBJ1, reply), "a flipped data bit passed the PEC");
    reply[0] ^= 0x01;

    /* The PEC covers the command too, so the same bytes can't pass as the
     * answer to a different question. */
    CHECK(!mlx_reply_ok(MLX_ADDR, MLX_RAM_TA, reply), "reply accepted for the wrong command");
}

/* Datasheet 4.1.8.2 examples. m°C is exact because 0.02 K is 20 m°C. */
static void test_mlx_conversion(void)
{
    static const struct { uint16_t raw; int32_t mc; } cases[] = {
        {0x3AF7, 28750},    /* "28.75 °C" */
        {0x3AF8, 28770},    /* "28.77 °C" */
        {0x7FFF, 382190},   /* "382.19 °C", the largest value it returns */
        {0x3AD2, 28010},    /* figure 6's reading */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int32_t mc = -1;
        bool ok = mlx_raw_to_mC(cases[i].raw, &mc);
        CHECK(ok && mc == cases[i].mc, "0x%04X gave %d m°C (ok=%d), want %d",
              cases[i].raw, mc, ok, cases[i].mc);
    }

    int32_t mc = 12345;
    CHECK(!mlx_raw_to_mC(0x8000 | 0x3AF7, &mc), "error flag not caught");
    CHECK(mc == 12345, "error flag set, but the output was still written");
}

static void test_mlx_fill(void)
{
    struct dsm_record r;
    record_init(&r, DSM_KIND_SAMPLE, DSM_SRC_CLOUDD);
    mlx_fill(&r, 0x3AF7, 0x3AF8);
    CHECK(r.v[0] == 28750 && r.v[1] == 28770, "v = %d %d", r.v[0], r.v[1]);
    CHECK(r.flags & DSM_FLAG_VALID, "a good reading isn't VALID");

    record_init(&r, DSM_KIND_SAMPLE, DSM_SRC_CLOUDD);
    mlx_fill(&r, 0x8000 | 0x3AF7, 0x3AF8);
    CHECK(!(r.flags & DSM_FLAG_VALID), "error flag on the sky reading, still VALID");

    record_init(&r, DSM_KIND_SAMPLE, DSM_SRC_CLOUDD);
    mlx_fill(&r, 0x3AF7, 0x8000 | 0x3AF8);
    CHECK(!(r.flags & DSM_FLAG_VALID), "error flag on the sensor reading, still VALID");
}

/* ---------------- TSL2591 ---------------- */

static void test_tsl_control_fields(void)
{
    /* AGAIN is bits 5:4. */
    CHECK(tsl_gain(0x00) == 1 && tsl_gain(0x10) == 25 && tsl_gain(0x20) == 428
          && tsl_gain(0x30) == 9876, "gains %d %d %d %d", tsl_gain(0x00),
          tsl_gain(0x10), tsl_gain(0x20), tsl_gain(0x30));

    /* ATIME is bits 2:0: 000 = 100 ms ... 101 = 600 ms; 110 and 111 reserved. */
    for (uint8_t a = 0; a <= 5; a++)
        CHECK(tsl_integration_ms(a) == 100 * (a + 1), "ATIME %u gave %d ms", a,
              tsl_integration_ms(a));
    CHECK(tsl_integration_ms(0x06) == 0 && tsl_integration_ms(0x07) == 0,
          "a reserved ATIME gave a time");

    /* Full scale is 37888 at 100 ms and 65535 for every longer time. */
    CHECK(tsl_max_count(0x00) == 37888, "100 ms max %u", tsl_max_count(0x00));
    for (uint8_t a = 1; a <= 5; a++)
        CHECK(tsl_max_count(a) == 65535, "ATIME %u max %u", a, tsl_max_count(a));

    /* The two fields don't leak into each other. */
    CHECK(tsl_integration_ms(0x31) == 200 && tsl_gain(0x35) == 9876,
          "AGAIN and ATIME bits mixed up");
}

static void test_tsl_fill(void)
{
    struct dsm_record r;

    /* Low byte first: CH0 = 0x1234, CH1 = 0x0578. Medium gain, 100 ms. */
    const uint8_t data[4] = {0x34, 0x12, 0x78, 0x05};
    record_init(&r, DSM_KIND_SAMPLE, DSM_SRC_SKYD);
    tsl_fill(&r, data, 0x10);
    CHECK(r.v[0] == 0x1234 && r.v[1] == 0x0578 && r.v[2] == 25 && r.v[3] == 100,
          "v = %d %d %d %d", r.v[0], r.v[1], r.v[2], r.v[3]);
    CHECK(r.flags & DSM_FLAG_VALID, "a good reading isn't VALID");

    /* CH0 = 0x9400 = 37888: full scale at 100 ms... */
    const uint8_t full[4] = {0x00, 0x94, 0x10, 0x00};
    record_init(&r, DSM_KIND_SAMPLE, DSM_SRC_SKYD);
    tsl_fill(&r, full, 0x10);
    CHECK(r.v[0] == 37888 && !(r.flags & DSM_FLAG_VALID), "saturated at 100 ms, still VALID");

    /* ...but not at 200 ms, where full scale is 65535. */
    record_init(&r, DSM_KIND_SAMPLE, DSM_SRC_SKYD);
    tsl_fill(&r, full, 0x11);
    CHECK(r.flags & DSM_FLAG_VALID, "37888 at 200 ms wrongly called saturated");

    /* The IR channel saturating counts too. */
    const uint8_t ir_full[4] = {0x00, 0x10, 0xFF, 0xFF};
    record_init(&r, DSM_KIND_SAMPLE, DSM_SRC_SKYD);
    tsl_fill(&r, ir_full, 0x11);
    CHECK(!(r.flags & DSM_FLAG_VALID), "IR channel saturated, still VALID");

    record_init(&r, DSM_KIND_SAMPLE, DSM_SRC_SKYD);
    tsl_fill(&r, data, 0x16);
    CHECK(!(r.flags & DSM_FLAG_VALID), "reserved ATIME, still VALID");
}

/* ---------------- BME280 ---------------- */

/* Realistic trimming values. Which chip they come from doesn't matter:
 * the tests below compare the datasheet's integer formulas with its
 * floating-point ones, which works for any realistic set. */
static const struct bme280_calib CAL = {
    .T1 = 27504, .T2 = 26435, .T3 = -1000,
    .P1 = 36477, .P2 = -10685, .P3 = 3024, .P4 = 2855, .P5 = 140, .P6 = -7,
    .P7 = 15500, .P8 = -14600, .P9 = 6000,
    .H1 = 75, .H2 = 362, .H3 = 0, .H4 = 313, .H5 = 50, .H6 = 30,
};

/* The datasheet's floating-point formulas (appendix 8.1), written out here
 * separately from convert.c. A typo in the integer code would make the two
 * disagree. */
static double ref_temp(const struct bme280_calib *c, int32_t adc_t)
{
    double var1 = ((double)adc_t / 16384.0 - (double)c->T1 / 1024.0) * (double)c->T2;
    double var2 = ((double)adc_t / 131072.0 - (double)c->T1 / 8192.0)
                * ((double)adc_t / 131072.0 - (double)c->T1 / 8192.0) * (double)c->T3;
    return (var1 + var2) / 5120.0;
}

static double ref_press(const struct bme280_calib *c, int32_t adc_p, int32_t t_fine)
{
    double var1 = (double)t_fine / 2.0 - 64000.0;
    double var2 = var1 * var1 * (double)c->P6 / 32768.0;
    var2 = var2 + var1 * (double)c->P5 * 2.0;
    var2 = var2 / 4.0 + (double)c->P4 * 65536.0;
    var1 = ((double)c->P3 * var1 * var1 / 524288.0 + (double)c->P2 * var1) / 524288.0;
    var1 = (1.0 + var1 / 32768.0) * (double)c->P1;
    if (var1 == 0.0)
        return 0;
    double p = 1048576.0 - (double)adc_p;
    p = (p - var2 / 4096.0) * 6250.0 / var1;
    var1 = (double)c->P9 * p * p / 2147483648.0;
    var2 = p * (double)c->P8 / 32768.0;
    return p + (var1 + var2 + (double)c->P7) / 16.0;
}

static double ref_hum(const struct bme280_calib *c, int32_t adc_h, int32_t t_fine)
{
    double h = (double)t_fine - 76800.0;
    h = (adc_h - ((double)c->H4 * 64.0 + (double)c->H5 / 16384.0 * h))
      * ((double)c->H2 / 65536.0 * (1.0 + (double)c->H6 / 67108864.0 * h
                                    * (1.0 + (double)c->H3 / 67108864.0 * h)));
    h = h * (1.0 - (double)c->H1 * h / 524288.0);
    return h > 100.0 ? 100.0 : h < 0.0 ? 0.0 : h;
}

/* A grid of raw readings across the range a backyard sees. Pressure and
 * humidity are compared using the integer t_fine on both sides, so each
 * formula is checked on its own. The integer outputs step in 0.01 °C,
 * 1/256 Pa and 1/1024 %RH, so they can't match to more than that. */
static void test_bme280_matches_float_formulas(void)
{
    static const int32_t adc_t[] = {400000, 470000, 519888, 560000, 600000};
    static const int32_t adc_p[] = {300000, 360000, 415148, 460000, 500000};
    static const int32_t adc_h[] = {20000, 26000, 30000, 35000, 40000};
    const size_t n = sizeof adc_t / sizeof adc_t[0];

    for (size_t i = 0; i < n; i++) {
        int32_t t_fine = 0;
        int32_t t = bme280_temp(&CAL, adc_t[i], &t_fine);
        double tr = ref_temp(&CAL, adc_t[i]);
        CHECK(absd(t / 100.0 - tr) <= 0.01, "adc_t %d: %.2f vs %.4f °C", adc_t[i], t / 100.0, tr);
        CHECK(t_fine != 0, "t_fine not set");

        for (size_t j = 0; j < n; j++) {
            uint32_t p = bme280_press(&CAL, adc_p[j], t_fine);
            double pr = ref_press(&CAL, adc_p[j], t_fine);
            CHECK(absd(p / 256.0 - pr) <= 1.0, "adc_p %d: %.2f vs %.2f Pa", adc_p[j], p / 256.0, pr);

            uint32_t h = bme280_hum(&CAL, adc_h[j], t_fine);
            double hr = ref_hum(&CAL, adc_h[j], t_fine);
            CHECK(absd(h / 1024.0 - hr) <= 0.01, "adc_h %d: %.4f vs %.4f %%RH", adc_h[j],
                  h / 1024.0, hr);
        }
    }
}

/* Lay a calibration out in the chip's byte order (table 16), the inverse
 * of what bme280_parse_calib does. */
static void put16(uint8_t *b, int v)
{
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void pack_calib(const struct bme280_calib *c, uint8_t lo[26], uint8_t hi[7])
{
    const int words[12] = {c->T1, c->T2, c->T3, c->P1, c->P2, c->P3,
                           c->P4, c->P5, c->P6, c->P7, c->P8, c->P9};
    memset(lo, 0, 26);
    for (int i = 0; i < 12; i++)
        put16(lo + 2 * i, words[i]);
    lo[24] = 0x00;   /* 0xA0 is not a calibration byte */
    lo[25] = c->H1;

    unsigned h4 = (unsigned)c->H4 & 0xFFF, h5 = (unsigned)c->H5 & 0xFFF;
    put16(hi, c->H2);
    hi[2] = c->H3;
    hi[3] = (uint8_t)(h4 >> 4);                          /* 0xE4 = H4[11:4] */
    hi[4] = (uint8_t)(((h5 & 0xF) << 4) | (h4 & 0xF));   /* 0xE5 = H5[3:0], H4[3:0] */
    hi[5] = (uint8_t)(h5 >> 4);                          /* 0xE6 = H5[11:4] */
    hi[6] = (uint8_t)c->H6;
}

static void check_calib_round_trip(const struct bme280_calib *want, const char *what)
{
    uint8_t lo[26], hi[7];
    struct bme280_calib got;
    pack_calib(want, lo, hi);
    bme280_parse_calib(&got, lo, hi);
    CHECK(got.T1 == want->T1 && got.T2 == want->T2 && got.T3 == want->T3, "%s: T", what);
    CHECK(got.P1 == want->P1 && got.P2 == want->P2 && got.P3 == want->P3
          && got.P4 == want->P4 && got.P5 == want->P5 && got.P6 == want->P6
          && got.P7 == want->P7 && got.P8 == want->P8 && got.P9 == want->P9, "%s: P", what);
    CHECK(got.H1 == want->H1 && got.H2 == want->H2 && got.H3 == want->H3, "%s: H1-H3", what);
    CHECK(got.H4 == want->H4, "%s: H4 %d, want %d", what, got.H4, want->H4);
    CHECK(got.H5 == want->H5, "%s: H5 %d, want %d", what, got.H5, want->H5);
    CHECK(got.H6 == want->H6, "%s: H6 %d, want %d", what, got.H6, want->H6);
}

/* H4 and H5 are 12-bit signed numbers sharing the byte at 0xE5, so the
 * negative case matters. */
static void test_bme280_parse_calib(void)
{
    check_calib_round_trip(&CAL, "typical");

    struct bme280_calib neg = CAL;
    neg.T3 = -32768;
    neg.H4 = -100;
    neg.H5 = -3;
    neg.H6 = -5;
    check_calib_round_trip(&neg, "negative");
}

static void test_bme280_parse_data(void)
{
    /* Pressure 0x655AC, temperature 0x7EED0, humidity 0x6C2B. The low 4
     * bits of each xlsb byte aren't data and must be ignored. */
    const uint8_t d[8] = {0x65, 0x5A, 0xCF, 0x7E, 0xED, 0x0F, 0x6C, 0x2B};
    int32_t p = 0, t = 0, h = 0;
    bme280_parse_data(d, &p, &t, &h);
    CHECK(p == 0x655AC, "pressure raw 0x%X", p);
    CHECK(t == 0x7EED0, "temperature raw 0x%X", t);
    CHECK(h == 0x6C2B, "humidity raw 0x%X", h);
}

static void test_bme280_fill(void)
{
    const uint8_t d[8] = {0x65, 0x5A, 0xC0, 0x7E, 0xED, 0x00, 0x6C, 0x2B};
    struct dsm_record r;
    record_init(&r, DSM_KIND_SAMPLE, DSM_SRC_ENVD);
    bme280_fill(&r, &CAL, d);

    int32_t t_fine = 0;
    int32_t t = bme280_temp(&CAL, 0x7EED0, &t_fine);
    uint32_t p = bme280_press(&CAL, 0x655AC, t_fine);
    uint32_t h = bme280_hum(&CAL, 0x6C2B, t_fine);
    CHECK(r.v[0] == t * 10, "m°C %d, want %d", r.v[0], t * 10);
    CHECK(r.v[1] == (int32_t)((h * 1000u + 512u) / 1024u), "milli-%%RH %d", r.v[1]);
    CHECK(r.v[2] == (int32_t)((p + 128u) / 256u), "Pa %d", r.v[2]);
    CHECK(r.v[0] > -40000 && r.v[0] < 85000 && r.v[1] >= 0 && r.v[1] <= 100000
          && r.v[2] > 30000 && r.v[2] < 110000, "values out of range: %d %d %d",
          r.v[0], r.v[1], r.v[2]);
    CHECK(r.flags & DSM_FLAG_VALID, "a good reading isn't VALID");

    /* Temperature still at its reset value 0x80000: that measurement never ran. */
    const uint8_t no_t[8] = {0x65, 0x5A, 0xC0, 0x80, 0x00, 0x00, 0x6C, 0x2B};
    record_init(&r, DSM_KIND_SAMPLE, DSM_SRC_ENVD);
    bme280_fill(&r, &CAL, no_t);
    CHECK(!(r.flags & DSM_FLAG_VALID), "temperature never measured, still VALID");

    /* Humidity at its reset value 0x8000 (humidity oversampling was off). */
    const uint8_t no_h[8] = {0x65, 0x5A, 0xC0, 0x7E, 0xED, 0x00, 0x80, 0x00};
    record_init(&r, DSM_KIND_SAMPLE, DSM_SRC_ENVD);
    bme280_fill(&r, &CAL, no_h);
    CHECK(!(r.flags & DSM_FLAG_VALID), "humidity never measured, still VALID");
}

void test_convert(void)
{
    test_mlx_pec_datasheet_examples();
    test_mlx_reply_check();
    test_mlx_conversion();
    test_mlx_fill();
    test_tsl_control_fields();
    test_tsl_fill();
    test_bme280_matches_float_formulas();
    test_bme280_parse_calib();
    test_bme280_parse_data();
    test_bme280_fill();
}
