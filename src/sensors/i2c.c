#include "sensors/i2c.h"

#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

int i2c_open(const char *dev)
{
    return open(dev, O_RDWR | O_CLOEXEC);
}

/* I2C_RDWR sends a list of messages as one transfer. */
static int transfer(int fd, struct i2c_msg *msgs, unsigned n)
{
    struct i2c_rdwr_ioctl_data data = { .msgs = msgs, .nmsgs = n };
    return ioctl(fd, I2C_RDWR, &data) < 0 ? -1 : 0;
}

int i2c_write_reg(int fd, uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    struct i2c_msg msg = { .addr = addr, .flags = 0, .len = 2, .buf = buf };
    return transfer(fd, &msg, 1);
}

int i2c_read_regs(int fd, uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    struct i2c_msg msgs[2] = {
        { .addr = addr, .flags = 0,        .len = 1,              .buf = &reg },
        { .addr = addr, .flags = I2C_M_RD, .len = (uint16_t)len,  .buf = buf },
    };
    return transfer(fd, msgs, 2);
}
