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
    bool broken;   /* a write or fsync failed, so we no longer know where the file ends */
    struct store_recovery recovery;
    struct store_counters counters;
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

/* Find where the good records end, and cut off everything after that.
 * A crash in the middle of a write leaves part of a record, or a block of
 * zeros, at the end of the file. We keep every record up to the first one
 * that is short or fails its CRC, and drop the rest. */
static enum store_status recover(int fd, off_t size, struct store_recovery *out)
{
    const off_t rec_size = (off_t)sizeof(struct dsm_record);
    off_t end = (off_t)sizeof(struct dsm_store_header);   /* end of the good part */
    uint64_t kept = 0;
    struct dsm_record r;

    while (size - end >= rec_size) {
        ssize_t n = pread(fd, &r, sizeof r, end);
        if (n < 0)
            return STORE_ERR_IO;   /* a failed read is not a bad record: cut nothing */
        if (n != rec_size || !record_intact(&r))
            break;
        kept++;
        end += rec_size;
    }

    out->kept = kept;
    out->cut_bytes = (uint64_t)(size - end);
    if (end == size)
        return STORE_OK;           /* clean: don't touch the file at all */

    /* fsync so the cut itself is on the card before anything new is
     * appended after it. */
    if (ftruncate(fd, end) != 0 || fsync(fd) != 0)
        return STORE_ERR_IO;
    return STORE_OK;
}

/* A file's name lives in its directory, and fsync on the file does not
 * save the directory. Until the directory is synced, a power cut can lose
 * the name of a new file, and with it every record in it. */
static enum store_status sync_parent_dir(const char *path)
{
    const char *slash = strrchr(path, '/');
    char *dir;
    if (slash == NULL)
        dir = strdup(".");
    else if (slash == path)
        dir = strdup("/");
    else
        dir = strndup(path, (size_t)(slash - path));
    if (dir == NULL)
        return STORE_ERR_IO;   /* errno is ENOMEM */

    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int saved_errno = errno;
    free(dir);
    if (fd < 0) {
        errno = saved_errno;
        return STORE_ERR_IO;
    }
    int rc = fsync(fd);
    saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return rc == 0 ? STORE_OK : STORE_ERR_IO;
}

enum store_status store_open(const char *path, struct dsm_store **out)
{
    enum store_status st;
    struct dsm_store *s = NULL;
    struct store_recovery rec = {0, 0};
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
     * was written), so it gets a header. Otherwise it must already be ours,
     * and only then is it safe to cut anything off its end. */
    if (sb.st_size == 0) {
        st = write_header(fd);
        if (st == STORE_OK && fsync(fd) != 0)
            st = STORE_ERR_IO;
    } else {
        st = check_header(fd, sb.st_size);
        if (st == STORE_OK)
            st = recover(fd, sb.st_size, &rec);
    }
    /* On every open, not just when the file was new: a crash between
     * creating the file and this sync leaves a file that looks old next
     * time, and its name would still not be on the card. */
    if (st == STORE_OK)
        st = sync_parent_dir(path);
    if (st != STORE_OK)
        goto fail;

    s = malloc(sizeof *s);
    if (s == NULL) {
        st = STORE_ERR_IO;     /* errno is ENOMEM */
        goto fail;
    }
    s->fd = fd;
    s->broken = false;
    s->recovery = rec;
    memset(&s->counters, 0, sizeof s->counters);
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
    s->counters.records++;
    s->counters.writes++;
    return STORE_OK;
}

enum store_status store_sync(struct dsm_store *s)
{
    if (s->broken)
        return STORE_ERR_BROKEN;

    /* No retry. On Linux a failed fsync can drop the dirty pages it could
     * not write, and a second fsync then reports success for data that
     * never reached the card. Only recovery on the next open can say what
     * is really in the file. */
    if (fsync(s->fd) != 0) {
        s->broken = true;
        return STORE_ERR_IO;
    }
    s->counters.fsyncs++;
    return STORE_OK;
}

/* Accepts NULL, like free(), so cleanup code can call it unconditionally. */
enum store_status store_close(struct dsm_store *s)
{
    if (s == NULL)
        return STORE_OK;

    enum store_status st = store_sync(s);
    int saved_errno = errno;   /* keep the sync's reason if it failed */
    int rc = close(s->fd);
    free(s);
    if (st != STORE_OK) {
        errno = saved_errno;
        return st;
    }
    return rc == 0 ? STORE_OK : STORE_ERR_IO;
}

struct store_recovery store_get_recovery(const struct dsm_store *s)
{
    return s->recovery;
}

struct store_counters store_get_counters(const struct dsm_store *s)
{
    return s->counters;
}
