#include "sensors/chips.h"
#include "sensors/convert.h"
#include "sensors/i2c.h"

#include <errno.h>
#include <time.h>

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

/* ---- TSL2591: medium gain (25x), 100 ms. ---- */

#define TSL_CONTROL_VALUE 0x10   /* AGAIN = 01 (medium), ATIME = 000 (100 ms) */

static int tsl_setup(int fd, uint8_t *id)
{
    if (i2c_read_regs(fd, TSL_ADDR, TSL_CMD | TSL_ID, id, 1) != 0)
        return SETUP_IO;
    if (*id != TSL_ID_VALUE)
        return SETUP_WRONG_ID;
    if (i2c_write_reg(fd, TSL_ADDR, TSL_CMD | TSL_CONTROL, TSL_CONTROL_VALUE) != 0
        || i2c_write_reg(fd, TSL_ADDR, TSL_CMD | TSL_ENABLE, TSL_PON | TSL_AEN) != 0)
        return SETUP_IO;
    sleep_ms(120);   /* let the first 100 ms integration finish */
    return SETUP_OK;
}

static int tsl_read(int fd, struct dsm_record *r)
{
    uint8_t status, data[4];
    if (i2c_read_regs(fd, TSL_ADDR, TSL_CMD | TSL_STATUS, &status, 1) != 0)
        return -1;
    if (!(status & TSL_AVALID)) {
        errno = EAGAIN;      /* no finished integration yet */
        return -1;
    }
    /* All four bytes in one read, so CH0 and CH1 come from the same cycle. */
    if (i2c_read_regs(fd, TSL_ADDR, TSL_CMD | TSL_C0DATAL, data, 4) != 0)
        return -1;
    tsl_fill(r, data, TSL_CONTROL_VALUE);
    return 0;
}

/* ---- MLX90614: no ID register; a good PEC on Ta is the check. ---- */

static int mlx_word(int fd, uint8_t cmd, uint16_t *raw)
{
    uint8_t reply[3];
    if (i2c_read_regs(fd, MLX_ADDR, cmd, reply, 3) != 0)
        return -1;
    if (!mlx_reply_ok(MLX_ADDR, cmd, reply)) {
        errno = EBADMSG;     /* the bytes were damaged on the bus */
        return -1;
    }
    *raw = (uint16_t)(reply[0] | (reply[1] << 8));
    return 0;
}

static int mlx_setup(int fd, uint8_t *id)
{
    uint16_t raw;
    *id = 0;
    return mlx_word(fd, MLX_RAM_TA, &raw) == 0 ? SETUP_OK : SETUP_IO;
}

static int mlx_read(int fd, struct dsm_record *r)
{
    uint16_t obj, amb;
    if (mlx_word(fd, MLX_RAM_TOBJ1, &obj) != 0 || mlx_word(fd, MLX_RAM_TA, &amb) != 0)
        return -1;
    mlx_fill(r, obj, amb);
    return 0;
}

/* ---- BME280: forced mode, 1x oversampling on all three. ---- */

static struct bme280_calib bme_cal;   /* read once at setup */

static int bme_setup(int fd, uint8_t *id)
{
    uint8_t lo[26], hi[7];
    if (i2c_read_regs(fd, BME_ADDR, BME_ID, id, 1) != 0)
        return SETUP_IO;
    if (*id != BME_ID_VALUE)
        return SETUP_WRONG_ID;
    if (i2c_read_regs(fd, BME_ADDR, BME_CALIB_LO, lo, sizeof lo) != 0
        || i2c_read_regs(fd, BME_ADDR, BME_CALIB_HI, hi, sizeof hi) != 0)
        return SETUP_IO;
    bme280_parse_calib(&bme_cal, lo, hi);
    return SETUP_OK;
}

static int bme_read(int fd, struct dsm_record *r)
{
    uint8_t d[8];
    /* ctrl_hum first: it only takes effect on the next ctrl_meas write.
     * ctrl_meas 0x25 = temperature x1, pressure x1, forced mode. */
    if (i2c_write_reg(fd, BME_ADDR, BME_CTRL_HUM, 0x01) != 0
        || i2c_write_reg(fd, BME_ADDR, BME_CTRL_MEAS, 0x25) != 0)
        return -1;
    sleep_ms(10);   /* max measurement time at x1/x1/x1 is 9.3 ms (appendix B) */
    if (i2c_read_regs(fd, BME_ADDR, BME_DATA, d, sizeof d) != 0)
        return -1;
    bme280_fill(r, &bme_cal, d);
    return 0;
}

const struct sensor_ops tsl_ops = { "skyd",   DSM_SRC_SKYD,   5000,  tsl_setup, tsl_read };
const struct sensor_ops mlx_ops = { "cloudd", DSM_SRC_CLOUDD, 1000,  mlx_setup, mlx_read };
const struct sensor_ops bme_ops = { "envd",   DSM_SRC_ENVD,   10000, bme_setup, bme_read };
