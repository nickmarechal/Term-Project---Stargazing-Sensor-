#include "store/store.h"
#include "common/crc32.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct dsm_store {
    int  fd;
    bool broken;   /* a write failed, so we no longer know where the file ends */
};

/* write() can write fewer bytes than asked (disk nearly full, or a signal
 * arrived), so keep going until everything is written or a real error. */
static int write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {          /* never expected for a file; don't loop forever */
            errno = EIO;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static uint64_t now_real_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;              /* 0 means "unknown"; the file still works */
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint32_t header_crc(const struct dsm_store_header *h)
{
    return dsm_crc32(h, offsetof(struct dsm_store_header, crc32));
}

static enum store_status write_header(int fd)
{
    struct dsm_store_header h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, DSM_STORE_MAGIC, sizeof h.magic);
    h.schema_ver = DSM_SCHEMA_VER;
    h.record_size = (uint16_t)sizeof(struct dsm_record);
    h.created_real_ns = now_real_ns();
    h.crc32 = header_crc(&h);
    return write_all(fd, &h, sizeof h) == 0 ? STORE_OK : STORE_ERR_IO;
}

/* Every check has to pass. If one fails this is not our file, and we
 * refuse to touch it: it could be somebody else's data. */
static enum store_status check_header(int fd, off_t size)
{
    struct dsm_store_header h;
    if (size < (off_t)sizeof h)
        return STORE_ERR_FORMAT;
    if (pread(fd, &h, sizeof h, 0) != (ssize_t)sizeof h)
        return STORE_ERR_IO;
    if (memcmp(h.magic, DSM_STORE_MAGIC, sizeof h.magic) != 0
        || h.crc32 != header_crc(&h)
        || h.schema_ver != DSM_SCHEMA_VER
        || h.record_size != sizeof(struct dsm_record))
        return STORE_ERR_FORMAT;
    return STORE_OK;
}

enum store_status store_open(const char *path, struct dsm_store **out)
{
    enum store_status st;
    struct dsm_store *s = NULL;
    struct stat sb;
    int saved_errno;

    *out = NULL;

    /* O_APPEND: every write goes to the current end of the file.
     * O_CLOEXEC: the fd is closed automatically if this process execs
     * another program, so it can't leak into children. */
    int fd = open(path, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return STORE_ERR_IO;

    if (fstat(fd, &sb) != 0) {
        st = STORE_ERR_IO;
        goto fail;
    }

    /* Empty means we just created it (or a crash came before the header
     * was written), so it gets a header. Otherwise it must already be ours. */
    st = sb.st_size == 0 ? write_header(fd) : check_header(fd, sb.st_size);
    if (st != STORE_OK)
        goto fail;

    s = malloc(sizeof *s);
    if (s == NULL) {
        st = STORE_ERR_IO;     /* errno is ENOMEM */
        goto fail;
    }
    s->fd = fd;
    s->broken = false;
    *out = s;
    return STORE_OK;

fail:
    saved_errno = errno;       /* close() could overwrite the real reason */
    close(fd);
    errno = saved_errno;
    return st;
}

enum store_status store_append(struct dsm_store *s, const struct dsm_record *r)
{
    if (s->broken)
        return STORE_ERR_BROKEN;

    /* Recovery will cut the file off at the first bad record, so one bad
     * record let in here would cost every good record after it. */
    if (!record_intact(r))
        return STORE_ERR_BADREC;

    /* If this fails part-way, part of a record may now be on disk, and
     * anything appended after it would be misaligned. Stop here instead. */
    if (write_all(s->fd, r, sizeof *r) != 0) {
        s->broken = true;
        return STORE_ERR_IO;
    }
    return STORE_OK;
}

/* Accepts NULL, like free(), so cleanup code can call it unconditionally. */
enum store_status store_close(struct dsm_store *s)
{
    if (s == NULL)
        return STORE_OK;
    int rc = close(s->fd);
    free(s);
    return rc == 0 ? STORE_OK : STORE_ERR_IO;
}
