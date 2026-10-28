/* sensorcheck: read every sensor once, with our own code, and print the
 * values. No socket and no stored, so it proves the hardware path on its
 * own. Exit 0 only if all three gave a VALID reading. */
#include "sensors/chips.h"
#include "sensors/i2c.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    const struct sensor_ops *all[] = { &tsl_ops, &mlx_ops, &bme_ops };
    int fd = i2c_open("/dev/i2c-1");
    if (fd < 0) {
        printf("open /dev/i2c-1: %s\n", strerror(errno));
        return 1;
    }

    int good = 0;
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        const struct sensor_ops *ops = all[i];
        struct dsm_record r;
        uint8_t id = 0;
        int st = ops->setup(fd, &id);
        if (st != SETUP_OK) {
            printf("%-7s setup failed: %s (id 0x%02x)\n", ops->name,
                   st == SETUP_WRONG_ID ? "wrong chip ID" : strerror(errno), id);
            continue;
        }
        record_init(&r, DSM_KIND_SAMPLE, ops->source);
        if (ops->read(fd, &r) != 0) {
            printf("%-7s read failed: %s\n", ops->name, strerror(errno));
            continue;
        }
        bool valid = r.flags & DSM_FLAG_VALID;
        good += valid;
        printf("%-7s v = %d %d %d %d  %s\n", ops->name, r.v[0], r.v[1], r.v[2], r.v[3],
               valid ? "VALID" : "NOT VALID");
    }
    close(fd);
    return good == 3 ? 0 : 1;
}
