/* stored: the only process that writes the data file.
 *
 *   stored <data-file> <socket-path> [sync-seconds]
 *
 * Opens the store (recovery runs), appends its own START record so every
 * restart shows in the file, then takes sealed records from the sensor
 * programs over an AF_UNIX SOCK_SEQPACKET socket (one record per message,
 * clients only send) and appends them. fsyncs every sync-seconds (default
 * 60), which bounds what a power cut can lose.
 *
 * Exit codes, for dskyd at M4:
 *   0  stopped by SIGTERM or SIGINT, everything synced
 *   1  bad arguments, a file that isn't ours, or the socket can't be set up
 *   2  a write or fsync failed: restart, and recovery runs on open
 *
 * Linux only: macOS does not support SOCK_SEQPACKET. */
#include "common/log.h"
#include "common/record.h"
#include "store/store.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum {
    MAX_CLIENTS    = 16,
    POLL_CAP_MS    = 1000,
    DEFAULT_SYNC_S = 60,
    DRAIN_MAX      = 1024,   /* records taken per client at shutdown */
    MSG_BUF        = 64,     /* bigger than a record, so a long message shows */
};

enum { EXIT_STOPPED = 0, EXIT_CONFIG = 1, EXIT_RUNTIME = 2 };

/* What reading one message did. */
enum read_result { READ_OK, READ_EMPTY, READ_GONE, READ_FATAL };

static volatile sig_atomic_t stop_requested;

static void on_stop(int sig)
{
    (void)sig;
    stop_requested = 1;
}

struct server {
    struct dsm_store *store;
    int listen_fd;
    int clients[MAX_CLIENTS];
    int nclients;
    uint64_t bad;              /* messages and records refused */
};

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);   /* can't fail for this clock */
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t now_ns(clockid_t clk)
{
    struct timespec ts;
    if (clock_gettime(clk, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* No SA_RESTART: a signal makes poll() return EINTR, so the loop sees the
 * flag. SIGPIPE is ignored; stored never writes to a socket anyway. */
static int install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_stop;
    if (sigaction(SIGTERM, &sa, NULL) != 0 || sigaction(SIGINT, &sa, NULL) != 0)
        return -1;
    sa.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &sa, NULL);
}

/* FD_CLOEXEC: no fd leaks into a program started later (dskyd restarts
 * things). O_NONBLOCK: accept() and recv() never wait, even at shutdown. */
static int set_fd_flags(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0)
        return -1;
    return fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static int open_listener(const char *path)
{
    struct sockaddr_un addr;
    struct stat sb;
    int fd, saved;
    size_t len = strlen(path);

    if (len >= sizeof addr.sun_path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    /* kill -9 leaves the socket file behind and bind() would fail on it,
     * so a stale socket goes. Anything else at that path is not ours. */
    if (lstat(path, &sb) == 0) {
        if (!S_ISSOCK(sb.st_mode)) {
            errno = EEXIST;
            return -1;
        }
        if (unlink(path) != 0)
            return -1;
    } else if (errno != ENOENT) {
        return -1;
    }

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, len + 1);

    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0)
        return -1;
    if (set_fd_flags(fd) != 0 || bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0)
        goto fail;
    if (listen(fd, MAX_CLIENTS) != 0) {
        saved = errno;
        unlink(path);          /* bind() made it */
        errno = saved;
        goto fail;
    }
    return fd;

fail:
    saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

static enum store_status append_start(struct dsm_store *s)
{
    struct dsm_record r;
    record_init(&r, DSM_KIND_START, DSM_SRC_STORED);
    r.t_mono_ns = now_ns(CLOCK_MONOTONIC);
    r.t_real_ns = now_ns(CLOCK_REALTIME);
    r.seq = 0;
    r.v[0] = 0;                /* run number: dskyd assigns it at M4 */
    record_seal(&r);
    return store_append(s, &r);
}

static void accept_clients(struct server *srv)
{
    for (;;) {
        int fd = accept(srv->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                log_msg(LVL_WARN, "accept_fail", "errno=%d", errno);
            return;
        }
        if (srv->nclients == MAX_CLIENTS) {
            log_msg(LVL_WARN, "too_many_clients", "max=%d", MAX_CLIENTS);
            close(fd);
            continue;
        }
        if (set_fd_flags(fd) != 0) {
            log_msg(LVL_WARN, "accept_fail", "errno=%d", errno);
            close(fd);
            continue;
        }
        srv->clients[srv->nclients++] = fd;
        log_msg(LVL_INFO, "client_new", "clients=%d", srv->nclients);
    }
}

static void drop_client(struct server *srv, int fd)
{
    for (int i = 0; i < srv->nclients; i++) {
        if (srv->clients[i] == fd) {
            close(fd);
            srv->clients[i] = srv->clients[--srv->nclients];
            log_msg(LVL_INFO, "client_gone", "clients=%d", srv->nclients);
            return;
        }
    }
}

/* Take one message from a client. Only a whole 48-byte record that passes
 * its CRC is appended; anything else is dropped and logged, and the client
 * stays connected (SEQPACKET keeps message boundaries, so one bad message
 * can't misalign the next). */
static enum read_result read_one(struct server *srv, int fd)
{
    unsigned char buf[MSG_BUF];
    struct dsm_record r;

    /* MSG_TRUNC: Linux returns the message's real length even when it
     * is longer than buf, so an oversized message is seen as such. */
    ssize_t n = recv(fd, buf, sizeof buf, MSG_TRUNC);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return READ_EMPTY;
        if (errno != ECONNRESET)
            log_msg(LVL_WARN, "recv_fail", "errno=%d", errno);
        return READ_GONE;
    }
    if (n == 0)
        return READ_GONE;      /* the client closed */
    if (n != (ssize_t)sizeof r) {
        log_msg(LVL_WARN, "bad_message", "len=%zd", n);
        srv->bad++;
        return READ_OK;
    }

    memcpy(&r, buf, sizeof r);
    switch (store_append(srv->store, &r)) {
    case STORE_OK:
        return READ_OK;
    case STORE_ERR_BADREC:
        log_msg(LVL_WARN, "bad_record", "source=%u seq=%" PRIu32, r.source_id, r.seq);
        srv->bad++;
        return READ_OK;
    default:
        log_msg(LVL_ERROR, "write_fail", "errno=%d", errno);
        return READ_FATAL;
    }
}

static int sync_now(struct server *srv)
{
    if (store_sync(srv->store) != STORE_OK) {
        log_msg(LVL_ERROR, "sync_fail", "errno=%d", errno);
        return -1;
    }
    struct store_counters c = store_get_counters(srv->store);
    log_msg(LVL_INFO, "sync", "records=%" PRIu64 " bad=%" PRIu64 " clients=%d fsyncs=%" PRIu64,
            c.records, srv->bad, srv->nclients, c.fsyncs);
    return 0;
}

/* After SIGTERM: take in what clients already sent, including from
 * connections not accepted yet. Their send() succeeded, so they count those
 * records as delivered. Bounded per client, so a client that keeps sending
 * can't hold the shutdown up. */
static int drain(struct server *srv)
{
    accept_clients(srv);
    for (int i = 0; i < srv->nclients; i++) {
        for (int k = 0; k < DRAIN_MAX; k++) {
            enum read_result r = read_one(srv, srv->clients[i]);
            if (r == READ_FATAL)
                return EXIT_RUNTIME;
            if (r != READ_OK)
                break;
        }
    }
    return EXIT_STOPPED;
}

/* Runs until SIGTERM/SIGINT. Returns the exit code. */
static int serve(struct server *srv, int64_t sync_ms)
{
    int64_t next_sync = mono_ms() + sync_ms;

    while (!stop_requested) {
        struct pollfd pfd[1 + MAX_CLIENTS];
        int npfd = 0;
        pfd[npfd++] = (struct pollfd){ .fd = srv->listen_fd, .events = POLLIN };
        for (int i = 0; i < srv->nclients; i++)
            pfd[npfd++] = (struct pollfd){ .fd = srv->clients[i], .events = POLLIN };

        /* Capped at 1 s: a signal that lands after the flag was checked but
         * before poll() started is then seen within a second, not at the
         * next sync. */
        int64_t wait = next_sync - mono_ms();
        if (wait < 0)
            wait = 0;
        if (wait > POLL_CAP_MS)
            wait = POLL_CAP_MS;

        if (poll(pfd, (nfds_t)npfd, (int)wait) < 0) {
            if (errno == EINTR)
                continue;
            log_msg(LVL_ERROR, "poll_fail", "errno=%d", errno);
            return EXIT_RUNTIME;
        }

        /* POLLHUP and POLLERR also go to read_one: recv() then reports
         * the close or the error. */
        for (int i = 1; i < npfd; i++) {
            if (pfd[i].revents == 0)
                continue;
            enum read_result r = read_one(srv, pfd[i].fd);
            if (r == READ_FATAL)
                return EXIT_RUNTIME;
            if (r == READ_GONE)
                drop_client(srv, pfd[i].fd);
        }
        if (pfd[0].revents & POLLIN)
            accept_clients(srv);

        if (mono_ms() >= next_sync) {
            if (sync_now(srv) != 0)
                return EXIT_RUNTIME;
            next_sync = mono_ms() + sync_ms;
        }
    }
    return drain(srv);
}

static int parse_secs(const char *s, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 1 || v > 3600)
        return -1;
    *out = (int)v;
    return 0;
}

int main(int argc, char **argv)
{
    struct server srv;
    int rc = EXIT_CONFIG;
    int sync_s = DEFAULT_SYNC_S;
    bool bound = false;

    memset(&srv, 0, sizeof srv);
    srv.listen_fd = -1;
    log_init("stored", stderr);

    if (argc < 3 || argc > 4 || (argc == 4 && parse_secs(argv[3], &sync_s) != 0)) {
        fprintf(stderr, "usage: stored <data-file> <socket-path> [sync-seconds, 1-3600]\n");
        return EXIT_CONFIG;
    }
    const char *data_path = argv[1], *sock_path = argv[2];

    if (install_signals() != 0) {
        log_msg(LVL_ERROR, "signal_fail", "errno=%d", errno);
        return EXIT_CONFIG;
    }

    enum store_status st = store_open(data_path, &srv.store);
    if (st != STORE_OK) {
        if (st == STORE_ERR_FORMAT)
            log_msg(LVL_ERROR, "open_fail", "file=%s reason=not_a_store_file", data_path);
        else
            log_msg(LVL_ERROR, "open_fail", "file=%s errno=%d", data_path, errno);
        return EXIT_CONFIG;
    }
    struct store_recovery rec = store_get_recovery(srv.store);
    if (rec.cut_bytes > 0)
        log_msg(LVL_WARN, "recover", "kept=%" PRIu64 " cut_bytes=%" PRIu64,
                rec.kept, rec.cut_bytes);
    log_msg(LVL_INFO, "start", "file=%s kept=%" PRIu64 " sync_s=%d", data_path, rec.kept, sync_s);

    if (append_start(srv.store) != STORE_OK) {
        log_msg(LVL_ERROR, "write_fail", "errno=%d", errno);
        rc = EXIT_RUNTIME;
        goto out;
    }

    srv.listen_fd = open_listener(sock_path);
    if (srv.listen_fd < 0) {
        log_msg(LVL_ERROR, "socket_fail", "path=%s errno=%d", sock_path, errno);
        rc = EXIT_CONFIG;
        goto out;
    }
    bound = true;

    rc = serve(&srv, (int64_t)sync_s * 1000);

out:
    for (int i = 0; i < srv.nclients; i++)
        close(srv.clients[i]);
    if (srv.listen_fd >= 0)
        close(srv.listen_fd);
    if (bound)
        unlink(sock_path);

    uint64_t records = store_get_counters(srv.store).records;
    st = store_close(srv.store);   /* syncs, unless a write already failed */
    if (rc == EXIT_STOPPED) {
        if (st != STORE_OK) {
            log_msg(LVL_ERROR, "sync_fail", "errno=%d", errno);
            rc = EXIT_RUNTIME;
        } else {
            log_msg(LVL_INFO, "stop", "records=%" PRIu64, records);
        }
    }
    return rc;
}
