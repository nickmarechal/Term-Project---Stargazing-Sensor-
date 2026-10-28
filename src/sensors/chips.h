#ifndef DSM_SENSORS_CHIPS_H
#define DSM_SENSORS_CHIPS_H

#include "common/record.h"

/* How to talk to each chip over I2C (Linux only). The math that turns its
 * bytes into numbers is in convert.c; this file only fetches the bytes. */

enum { SETUP_OK = 0, SETUP_IO = -1, SETUP_WRONG_ID = -2 };

struct sensor_ops {
    const char *name;     /* process name, used in every log line */
    uint16_t source;      /* enum dsm_source */
    int period_ms;        /* one SAMPLE this often */
    /* Check the chip ID and configure it. SETUP_OK, SETUP_IO (errno set) or
     * SETUP_WRONG_ID (*id holds what the chip said). */
    int (*setup)(int fd, uint8_t *id);
    /* One reading into r's payload and flags. 0, or -1 with errno set. */
    int (*read)(int fd, struct dsm_record *r);
};

extern const struct sensor_ops tsl_ops;   /* skyd */
extern const struct sensor_ops mlx_ops;   /* cloudd */
extern const struct sensor_ops bme_ops;   /* envd */

#endif
