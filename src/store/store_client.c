/* The sending half of the stored protocol. Linux only (SOCK_SEQPACKET);
 * the contract is in store_client.h. */
#include "store/store_client.h"
#include "common/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum { RECONNECT_MS = 1000 };

struct store_client {
    struct sockaddr_un addr;
    int fd;                     /* -1 when not connected */
    bool tried;                 /* last_try is set */
    struct timespec last_try;   /* CLOCK_MONOTONIC of the last connect attempt */
    bool down_logged;           /* "can't reach stored" already logged; don't repeat it */
    bool full_logged;           /* "stored's queue is full" already logged */
    uint64_t dropped;
};

struct store_client *store_client_new(const char *socket_path)
{
    struct store_client *c;
    size_t len = strlen(socket_path);
    if (len >= sizeof c->addr.sun_path) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    c = malloc(sizeof *c);
    if (c == NULL)
        return NULL;           /* errno is ENOMEM */
    memset(c, 0, sizeof *c);
    c->addr.sun_family = AF_UNIX;
    memcpy(c->addr.sun_path, socket_path, len + 1);
    c->fd = -1;
    return c;
}

static long ms_between(const struct timespec *a, const struct timespec *b)
{
    return (long)(b->tv_sec - a->tv_sec) * 1000 + (b->tv_nsec - a->tv_nsec) / 1000000;
}

/* At most one attempt per second, so a sensor program whose stored is
 * down spends one failed connect() a second on it, not one per sample. */
static bool try_connect(struct store_client *c)
{
    struct timespec now;
    int fd = -1, fl, e;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return false;          /* can't rate-limit, so don't try */
    if (c->tried && ms_between(&c->last_try, &now) < RECONNECT_MS)
        return false;
    c->tried = true;
    c->last_try = now;

    /* O_NONBLOCK: connect() and send() fail with EAGAIN instead of waiting
     * when stored's queue is full. FD_CLOEXEC: not inherited by children. */
    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0)
        goto fail;
    fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0
        || fcntl(fd, F_SETFD, FD_CLOEXEC) != 0
        || connect(fd, (const struct sockaddr *)&c->addr, sizeof c->addr) != 0)
        goto fail;

    c->fd = fd;
    c->down_logged = false;
    log_msg(LVL_INFO, "store_connect", "path=%s dropped=%llu", c->addr.sun_path,
            (unsigned long long)c->dropped);
    return true;

fail:
    e = errno;
    if (fd >= 0)
        close(fd);
    if (!c->down_logged) {
        log_msg(LVL_WARN, "store_connect_fail", "path=%s errno=%d", c->addr.sun_path, e);
        c->down_logged = true;
    }
    return false;
}

int store_client_send(struct store_client *c, const struct dsm_record *r)
{
    ssize_t n;

    if (c->fd < 0 && !try_connect(c))
        goto drop;

    do
        n = send(c->fd, r, sizeof *r, MSG_NOSIGNAL);
    while (n < 0 && errno == EINTR);

    if (n == (ssize_t)sizeof *r) {
        c->full_logged = false;
        return 0;
    }
    if (n >= 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
        /* stored is alive but behind: drop this one, stay connected. */
        if (!c->full_logged) {
            log_msg(LVL_WARN, "store_full", "errno=%d", n >= 0 ? 0 : errno);
            c->full_logged = true;
        }
        goto drop;
    }

    /* EPIPE, ECONNRESET, ...: stored is gone. Reconnect on a later send. */
    log_msg(LVL_WARN, "store_disconnect", "errno=%d", errno);
    close(c->fd);
    c->fd = -1;
    c->down_logged = true;     /* the failed reconnects that follow need no line each */

drop:
    c->dropped++;
    return -1;
}

uint64_t store_client_dropped(const struct store_client *c)
{
    return c->dropped;
}

void store_client_free(struct store_client *c)
{
    if (c == NULL)
        return;
    if (c->fd >= 0)
        close(c->fd);
    free(c);
}
