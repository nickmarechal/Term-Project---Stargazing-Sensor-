#include "sensors/i2c.h"

#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int i2c_open(const char *dev)
{
    return open(dev, O_RDWR | O_CLOEXEC);
}

/* I2C_RDWR sends a list of messages as one transfer.
 *
 * Every struct handed to the kernel is memset to zero first. The compiler
 * puts invisible padding bytes inside these structs, and "= { .field = x }"
 * doesn't promise to zero them, so without the memset uninitialized bytes
 * would reach the kernel. valgrind reported exactly that (2026-10-28). */
static int transfer(int fd, struct i2c_msg *msgs, unsigned n)
{
    struct i2c_rdwr_ioctl_data data;
    memset(&data, 0, sizeof data);
    data.msgs = msgs;
    data.nmsgs = n;
    return ioctl(fd, I2C_RDWR, &data) < 0 ? -1 : 0;
}

static void set_msg(struct i2c_msg *m, uint8_t addr, uint16_t flags, uint8_t *buf, size_t len)
{
    memset(m, 0, sizeof *m);
    m->addr = addr;
    m->flags = flags;
    m->len = (uint16_t)len;
    m->buf = buf;
}

int i2c_write_reg(int fd, uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    struct i2c_msg msg;
    set_msg(&msg, addr, 0, buf, sizeof buf);
    return transfer(fd, &msg, 1);
}

int i2c_read_regs(int fd, uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    struct i2c_msg msgs[2];
    set_msg(&msgs[0], addr, 0, &reg, 1);          /* send the register number */
    set_msg(&msgs[1], addr, I2C_M_RD, buf, len);  /* then read, no stop between */
    return transfer(fd, msgs, 2);
}
