#ifndef DSM_SENSORS_I2C_H
#define DSM_SENSORS_I2C_H

#include <stddef.h>
#include <stdint.h>

/* Linux only (<linux/i2c-dev.h>). Every call returns 0 on success, or -1
 * with errno set. */

int i2c_open(const char *dev);   /* returns an fd, or -1 */

/* Write one byte to register reg of the chip at addr. */
int i2c_write_reg(int fd, uint8_t addr, uint8_t reg, uint8_t val);

/* Send reg, then read len bytes, as one combined transfer (a repeated
 * start, not a stop and a new start). The MLX90614 requires this. */
int i2c_read_regs(int fd, uint8_t addr, uint8_t reg, uint8_t *buf, size_t len);

#endif
