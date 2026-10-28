/* Integration test for stored and store_client (Linux only: SOCK_SEQPACKET).
 *
 *   itest_stored <path-to-stored>
 *
 * Starts the real stored binary on files in a fresh temporary directory and
 * talks to it the way the sensor programs will: raw SEQPACKET sockets for
 * the protocol cases, store_client for the client cases. stored's stderr
 * goes to one log file per run, which the checks read. All records here are
 * made up by the test; nothing comes from a sensor. */
#include "check.h"
#include "common/log.h"
#include "common/record.h"
#include "store/store.h"
#include "store/store_client.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int check_count;
int check_failures;

enum {
    HDR = sizeof(struct dsm_store_header),
    REC = sizeof(struct dsm_record),
    MAX_FILE_RECS = 4096,
    READY_MS = 20000,   /* generous: under valgrind stored starts slowly */
    MAX_NAMES = 32,
};

static const char *stored_bin;
static char dir[64];
static char sock[128];

/* Every file the test makes, so they can be removed at the end. */
static char names[MAX_NAMES][128];
static int nnames;

/* A path inside the test directory, remembered for cleanup. */
static const char *tmp_path(const char *name)
{
    if (nnames == MAX_NAMES) {
        fprintf(stderr, "itest: too many temporary files\n");
        exit(2);
    }
    char *p = names[nnames++];
    snprintf(p, sizeof names[0], "%s/%s", dir, name);
    return p;
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Run stored with these arguments (after argv[0]), stderr to log. */
static pid_t spawn(const char *log, const char *a1, const char *a2, const char *a3)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;            /* parent, or -1 */

    int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0)
        dup2(fd, 2);
    char *argv[] = { (char *)stored_bin, (char *)a1, (char *)a2, (char *)a3, NULL };
    execv(stored_bin, argv);
    _exit(127);
}

static pid_t start_stored(const char *log, const char *file)
{
    return spawn(log, file, sock, NULL);
}

/* Exit code, 128 + signal if it was killed, or -1 if it is still running
 * after timeout_ms. */
static int wait_exit(pid_t pid, int timeout_ms)
{
    double end = now_ms() + timeout_ms;
    for (;;) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        if (r < 0 || now_ms() > end)
            return -1;
        sleep_ms(5);
    }
}

/* Print a stored log, for a check that failed. */
static void show_log(const char *log)
{
    char buf[4096];
    FILE *f = fopen(log, "r");
    if (f == NULL)
        return;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    fprintf(stderr, "  --- %s ---\n%s  ---\n", log, buf);
}

static int stop_stored(pid_t pid, const char *log)
{
    kill(pid, SIGTERM);
    int code = wait_exit(pid, READY_MS);
    if (code == -1) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    if (code != 0)
        show_log(log);
    return code;
}

static void kill9(pid_t pid)
{
    kill(pid, SIGKILL);
    int code = wait_exit(pid, READY_MS);
    CHECK(code == 128 + SIGKILL, "kill -9: stored ended with %d", code);
}

/* A raw SEQPACKET connection to stored, retried until it is listening or
 * timeout_ms has passed. FD_CLOEXEC, so the next stored we fork doesn't
 * inherit it. Returns the fd or -1. */
static int connect_raw(int timeout_ms)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, sock, strlen(sock) + 1);

    double end = now_ms() + timeout_ms;
    for (;;) {
        int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (fd < 0)
            return -1;
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) == 0
            && connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0)
            return fd;
        close(fd);
        if (now_ms() > end)
            return -1;
        sleep_ms(10);
    }
}

/* Block until stored accepts connections. Returns 1 if it does, 0 if it
 * exited first (WNOWAIT leaves its exit code to be collected later) or
 * READY_MS passed. */
static int wait_ready(pid_t pid)
{
    double end = now_ms() + READY_MS;
    for (;;) {
        int fd = connect_raw(0);
        if (fd >= 0) {
            close(fd);
            return 1;
        }
        siginfo_t si;
        memset(&si, 0, sizeof si);
        if (waitid(P_PID, (id_t)pid, &si, WEXITED | WNOHANG | WNOWAIT) == 0 && si.si_pid == pid)
            return 0;
        if (now_ms() > end)
            return 0;
        sleep_ms(10);
    }
}

static int send_raw(int fd, const void *buf, size_t len)
{
    return send(fd, buf, len, MSG_NOSIGNAL) == (ssize_t)len;
}

/* A sealed SAMPLE that says where it came from. */
static struct dsm_record make_rec(uint16_t src, uint32_t seq)
{
    struct dsm_record r;
    record_init(&r, DSM_KIND_SAMPLE, src);
    r.flags = DSM_FLAG_SYNTHETIC;   /* made up by this test */
    r.seq = seq;
    r.v[0] = (int32_t)seq * 10;
    record_seal(&r);
    return r;
}

static int send_rec(int fd, uint16_t src, uint32_t seq)
{
    struct dsm_record r = make_rec(src, seq);
    return send_raw(fd, &r, sizeof r);
}

/* What a store file holds. */
struct contents {
    int ok;              /* header present and ours */
    size_t n;            /* whole records after the header */
    size_t tail;         /* bytes after the last whole record */
    size_t bad;          /* whole records that fail their CRC */
    struct dsm_record r[MAX_FILE_RECS];
};

static struct contents file_contents;

static const struct contents *load(const char *file)
{
    struct contents *c = &file_contents;
    memset(c, 0, sizeof *c);
    FILE *f = fopen(file, "rb");
    if (f == NULL)
        return c;
    struct dsm_store_header h;
    if (fread(&h, 1, sizeof h, f) == sizeof h
        && memcmp(h.magic, DSM_STORE_MAGIC, sizeof h.magic) == 0)
        c->ok = 1;
    unsigned char buf[REC];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) == sizeof buf && c->n < MAX_FILE_RECS) {
        memcpy(&c->r[c->n], buf, sizeof buf);
        if (!record_intact(&c->r[c->n]))
            c->bad++;
        c->n++;
    }
    c->tail = got < sizeof buf ? got : 0;
    fclose(f);
    return c;
}

static long size_of(const char *file)
{
    struct stat sb;
    return stat(file, &sb) == 0 ? (long)sb.st_size : -1;
}

/* Wait until the file holds n records: stored has taken in what was sent. */
static int wait_records(const char *file, size_t n)
{
    double end = now_ms() + READY_MS;
    while (size_of(file) < (long)(HDR + n * REC)) {
        if (now_ms() > end)
            return 0;
        sleep_ms(10);
    }
    return 1;
}

static int log_has(const char *log, const char *needle)
{
    static char buf[65536];
    FILE *f = fopen(log, "r");
    if (f == NULL)
        return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static int is_start(const struct dsm_record *r)
{
    return r->kind == DSM_KIND_START && r->source_id == DSM_SRC_STORED && r->seq == 0;
}

/* 1. Two clients, 25 records each, then SIGTERM right away. Every record
 *    whose send() succeeded must be in the file, even ones stored had not
 *    read yet when the signal came: the clients count them as delivered. */
static void test_two_clients_clean_stop(void)
{
    const char *file = tmp_path("two.dsm"), *log = tmp_path("two.log");
    pid_t pid = start_stored(log, file);
    CHECK(pid > 0 && wait_ready(pid), "stored did not start");
    int a = connect_raw(1000), b = connect_raw(1000);
    CHECK(a >= 0 && b >= 0, "could not connect two clients");
    int sent = 0;
    for (uint32_t i = 0; i < 25; i++)
        sent += send_rec(a, DSM_SRC_SKYD, i) + send_rec(b, DSM_SRC_CLOUDD, i);
    CHECK(sent == 50, "%d of 50 sends succeeded", sent);

    CHECK(stop_stored(pid, log) == 0, "SIGTERM: exit code is not 0");
    close(a);
    close(b);

    const struct contents *c = load(file);
    CHECK(c->ok, "no store header");
    CHECK(c->n == 51 && c->tail == 0 && c->bad == 0,
          "file has %zu records (want 51: START + 50), tail %zu, bad %zu", c->n, c->tail, c->bad);
    CHECK(c->n > 0 && is_start(&c->r[0]), "first record is not stored's START");
    uint32_t next_sky = 0, next_cloud = 0;
    for (size_t i = 1; i < c->n; i++) {
        uint32_t *next = c->r[i].source_id == DSM_SRC_SKYD ? &next_sky : &next_cloud;
        CHECK(c->r[i].seq == *next, "record %zu: source %u seq %u, want seq %u",
              i, c->r[i].source_id, c->r[i].seq, *next);
        *next = c->r[i].seq + 1;
    }
    CHECK(log_has(log, " INFO start ") && log_has(log, " INFO stop records=51"),
          "start/stop lines missing from the log");
    CHECK(access(sock, F_OK) != 0, "socket file left behind after a clean stop");
}

/* 2 and 3. A short message, a long one and an unsealed record are dropped
 *    and logged; the client stays connected and stored keeps running. */
static void test_bad_input_dropped(void)
{
    const char *file = tmp_path("bad.dsm"), *log = tmp_path("bad.log");
    pid_t pid = start_stored(log, file);
    CHECK(pid > 0 && wait_ready(pid), "stored did not start");
    int fd = connect_raw(1000);
    CHECK(fd >= 0, "could not connect");

    unsigned char junk[100];
    memset(junk, 0x5A, sizeof junk);
    struct dsm_record unsealed = make_rec(DSM_SRC_SKYD, 5);
    unsealed.v[1] = 1;   /* changed after sealing */

    CHECK(send_raw(fd, junk, 30), "send of 30 bytes failed");
    CHECK(send_rec(fd, DSM_SRC_SKYD, 1), "send after the short message failed");
    CHECK(send_raw(fd, junk, sizeof junk), "send of 100 bytes failed");
    CHECK(send_raw(fd, &unsealed, sizeof unsealed), "send of the unsealed record failed");
    CHECK(send_rec(fd, DSM_SRC_SKYD, 2), "send after the bad record failed");
    CHECK(wait_records(file, 3), "the good records never reached the file");
    CHECK(wait_exit(pid, 0) == -1, "stored exited after bad input");

    CHECK(stop_stored(pid, log) == 0, "SIGTERM: exit code is not 0");
    close(fd);

    const struct contents *c = load(file);
    CHECK(c->n == 3 && c->bad == 0 && c->tail == 0,
          "file has %zu records (want START, seq 1, seq 2), bad %zu, tail %zu",
          c->n, c->bad, c->tail);
    CHECK(c->n == 3 && c->r[1].seq == 1 && c->r[2].seq == 2, "wrong records kept");
    CHECK(log_has(log, "WARN bad_message len=30"), "30-byte message not logged");
    CHECK(log_has(log, "WARN bad_message len=100"), "100-byte message not logged");
    CHECK(log_has(log, "WARN bad_record source=1 seq=5"), "unsealed record not logged");
}

/* 4. kill -9, then 20 junk bytes appended (a torn tail, provoked by this
 *    test). The restart must cut exactly those 20 bytes, keep every record,
 *    and get past the socket file kill -9 left behind. */
static void test_kill9_torn_tail(void)
{
    const char *file = tmp_path("torn.dsm"), *log1 = tmp_path("torn1.log"),
               *log2 = tmp_path("torn2.log");
    pid_t pid = start_stored(log1, file);
    CHECK(pid > 0 && wait_ready(pid), "stored did not start");
    int fd = connect_raw(1000);
    for (uint32_t i = 0; i < 10; i++)
        send_rec(fd, DSM_SRC_CLOUDD, i);
    CHECK(wait_records(file, 11), "the 10 records never reached the file");
    kill9(pid);
    close(fd);
    CHECK(access(sock, F_OK) == 0, "no stale socket after kill -9; the restart proves less");

    FILE *f = fopen(file, "ab");
    CHECK(f != NULL, "could not open %s", file);
    if (f != NULL) {
        unsigned char junk[20];
        memset(junk, 0xAB, sizeof junk);
        fwrite(junk, 1, sizeof junk, f);
        fclose(f);
    }

    pid = start_stored(log2, file);
    CHECK(pid > 0 && wait_ready(pid), "stored did not restart");
    CHECK(stop_stored(pid, log2) == 0, "SIGTERM: exit code is not 0");
    CHECK(log_has(log2, "WARN recover kept=11 cut_bytes=20"), "recovery not logged as expected");
    const struct contents *c = load(file);
    CHECK(c->n == 12 && c->bad == 0 && c->tail == 0,
          "file has %zu records (want 12), bad %zu, tail %zu", c->n, c->bad, c->tail);
    CHECK(c->n == 12 && is_start(&c->r[11]), "the restart's START is not last");
}

/* 5. A store_client keeps sending through a stored crash and restart.
 *    While stored is down, sends return -1 at once and count as dropped;
 *    after the restart they succeed again. */
static void test_client_through_restart(void)
{
    const char *file = tmp_path("restart.dsm"), *log1 = tmp_path("restart1.log"),
               *log2 = tmp_path("restart2.log");
    struct store_client *cl = store_client_new(sock);
    CHECK(cl != NULL, "store_client_new failed");
    if (cl == NULL)
        return;

    pid_t pid = start_stored(log1, file);
    CHECK(pid > 0 && wait_ready(pid), "stored did not start");
    struct dsm_record r = make_rec(DSM_SRC_CLOUDD, 0);
    CHECK(store_client_send(cl, &r) == 0, "send to a running stored failed");
    CHECK(wait_records(file, 2), "record 0 never reached the file");
    kill9(pid);

    int dropped = 0;
    for (uint32_t i = 1; i <= 4; i++) {
        r = make_rec(DSM_SRC_CLOUDD, i);
        double t0 = now_ms();
        dropped += store_client_send(cl, &r) == -1;
        CHECK(now_ms() - t0 < 200, "send %u took %.0f ms with stored down", i, now_ms() - t0);
    }
    CHECK(dropped == 4, "%d of 4 sends dropped while stored was down", dropped);
    CHECK(store_client_dropped(cl) == 4, "dropped counter %" PRIu64 ", want 4",
          store_client_dropped(cl));

    pid = start_stored(log2, file);
    CHECK(pid > 0 && wait_ready(pid), "stored did not restart");
    sleep_ms(1100);            /* past the one-reconnect-per-second limit */
    int sent = 0;
    for (uint32_t i = 5; i < 10; i++) {
        r = make_rec(DSM_SRC_CLOUDD, i);
        sent += store_client_send(cl, &r) == 0;
    }
    CHECK(sent == 5, "%d of 5 sends after the restart succeeded", sent);
    CHECK(stop_stored(pid, log2) == 0, "SIGTERM: exit code is not 0");
    store_client_free(cl);

    /* START, 0, START, 5..9: seq 1-4 are the visible gap. */
    const struct contents *c = load(file);
    CHECK(c->n == 8 && c->bad == 0 && c->tail == 0,
          "file has %zu records (want 8), bad %zu, tail %zu", c->n, c->bad, c->tail);
    if (c->n == 8) {
        CHECK(is_start(&c->r[0]) && c->r[1].seq == 0 && is_start(&c->r[2]),
              "the first run's records are wrong");
        for (uint32_t i = 3; i < 8; i++)
            CHECK(c->r[i].seq == i + 2, "record %u has seq %u, want %u", i, c->r[i].seq, i + 2);
    }
}

/* 5b. At most one connect attempt per second. The socket is renamed away
 *     so a connect fails, then put back: an immediate retry must still be
 *     refused without trying, and a retry a second later must connect. */
static void test_client_reconnect_rate(void)
{
    const char *file = tmp_path("rate.dsm"), *log = tmp_path("rate.log"),
               *moved = tmp_path("moved.sock");
    pid_t pid = start_stored(log, file);
    CHECK(pid > 0 && wait_ready(pid), "stored did not start");
    CHECK(rename(sock, moved) == 0, "rename of the socket failed");

    struct store_client *cl = store_client_new(sock);
    CHECK(cl != NULL, "store_client_new failed");
    if (cl == NULL)
        return;
    struct dsm_record r = make_rec(DSM_SRC_SKYD, 0);
    CHECK(store_client_send(cl, &r) == -1, "send with no socket file did not fail");

    CHECK(rename(moved, sock) == 0, "rename back failed");
    r = make_rec(DSM_SRC_SKYD, 1);
    CHECK(store_client_send(cl, &r) == -1, "reconnected within a second of the last attempt");
    sleep_ms(1100);
    r = make_rec(DSM_SRC_SKYD, 2);
    CHECK(store_client_send(cl, &r) == 0, "did not reconnect a second later");
    CHECK(store_client_dropped(cl) == 2, "dropped %" PRIu64 ", want 2", store_client_dropped(cl));
    store_client_free(cl);
    CHECK(stop_stored(pid, log) == 0, "SIGTERM: exit code is not 0");
}

/* 5c. A stored that is alive but not reading (SIGSTOP) must never stall a
 *     sender: once its queue is full, sends fail at once and are counted.
 *     After SIGCONT the same connection works again. */
static void test_client_never_blocks(void)
{
    const char *file = tmp_path("stop.dsm"), *log = tmp_path("stop.log");
    pid_t pid = start_stored(log, file);
    CHECK(pid > 0 && wait_ready(pid), "stored did not start");
    struct store_client *cl = store_client_new(sock);
    CHECK(cl != NULL, "store_client_new failed");
    if (cl == NULL)
        return;
    struct dsm_record r = make_rec(DSM_SRC_CLOUDD, 0);
    CHECK(store_client_send(cl, &r) == 0, "first send failed");
    CHECK(wait_records(file, 2), "record 0 never reached the file");

    kill(pid, SIGSTOP);
    double worst = 0;
    int sent = 1;
    for (uint32_t i = 1; i <= 2000; i++) {
        r = make_rec(DSM_SRC_CLOUDD, i);
        double t0 = now_ms();
        sent += store_client_send(cl, &r) == 0;
        double dt = now_ms() - t0;
        if (dt > worst)
            worst = dt;
    }
    kill(pid, SIGCONT);

    CHECK(store_client_dropped(cl) > 0, "2000 sends to a stopped stored: none dropped");
    CHECK(worst < 200, "slowest send took %.0f ms; a send must never wait", worst);
    CHECK(wait_records(file, 1 + (size_t)sent), "the queued records never reached the file");
    r = make_rec(DSM_SRC_CLOUDD, 2001);
    CHECK(store_client_send(cl, &r) == 0, "send after SIGCONT failed");
    sent++;
    store_client_free(cl);
    CHECK(stop_stored(pid, log) == 0, "SIGTERM: exit code is not 0");

    const struct contents *c = load(file);
    CHECK(c->n == 1 + (size_t)sent && c->bad == 0 && c->tail == 0,
          "file has %zu records, want %d; bad %zu, tail %zu", c->n, 1 + sent, c->bad, c->tail);
}

/* 6. Files and arguments stored must refuse, exiting 1 and touching nothing. */
static void test_refusals(void)
{
    const char *file = tmp_path("foreign.dsm"), *log = tmp_path("refuse.log");
    const char text[] = "somebody's notes, not a store file\n";
    FILE *f = fopen(file, "wb");
    CHECK(f != NULL, "could not create %s", file);
    if (f != NULL) {
        fwrite(text, 1, sizeof text - 1, f);
        fclose(f);
    }

    pid_t pid = start_stored(log, file);
    CHECK(wait_exit(pid, READY_MS) == 1, "a foreign file: exit code is not 1");
    char back[sizeof text];
    memset(back, 0, sizeof back);
    f = fopen(file, "rb");
    size_t n = f != NULL ? fread(back, 1, sizeof back, f) : 0;
    if (f != NULL)
        fclose(f);
    CHECK(n == sizeof text - 1 && memcmp(back, text, n) == 0, "the foreign file was modified");
    CHECK(log_has(log, "ERROR open_fail"), "refusal not logged");

    /* A socket path that is a regular file must not be deleted. */
    const char *data = tmp_path("ok.dsm");
    pid = spawn(log, data, file, NULL);
    CHECK(wait_exit(pid, READY_MS) == 1, "socket path is a regular file: exit code is not 1");
    CHECK(size_of(file) == (long)(sizeof text - 1), "the file at the socket path was touched");

    pid = spawn(log, data, NULL, NULL);
    CHECK(wait_exit(pid, READY_MS) == 1, "missing argument: exit code is not 1");
    pid = spawn(log, data, sock, "0");
    CHECK(wait_exit(pid, READY_MS) == 1, "sync interval 0: exit code is not 1");

    char longpath[200];
    memset(longpath, 'x', sizeof longpath - 1);
    longpath[sizeof longpath - 1] = '\0';
    errno = 0;
    CHECK(store_client_new(longpath) == NULL && errno == ENAMETOOLONG,
          "store_client_new accepted a path too long for sockaddr_un");
}

/* The periodic fsync, with a 1 s interval so the test doesn't wait 60 s. */
static void test_periodic_sync(void)
{
    const char *file = tmp_path("sync.dsm"), *log = tmp_path("sync.log");
    pid_t pid = spawn(log, file, sock, "1");
    CHECK(pid > 0 && wait_ready(pid), "stored did not start");
    int fd = connect_raw(1000);
    send_rec(fd, DSM_SRC_ENVD, 0);
    double end = now_ms() + READY_MS;
    while (!log_has(log, " INFO sync ") && now_ms() < end)
        sleep_ms(50);
    CHECK(log_has(log, " INFO sync records="), "no sync line within the timeout");
    CHECK(log_has(log, "fsyncs=1"), "the first sync line does not say fsyncs=1");
    close(fd);
    CHECK(stop_stored(pid, log) == 0, "SIGTERM: exit code is not 0");
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: itest_stored <path-to-stored>\n");
        return 2;
    }
    stored_bin = argv[1];
    alarm(600);                /* a hang fails the run instead of blocking make */
    log_init("itest_stored", stderr);

    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/dsm-itest-XXXXXX", tmp != NULL && *tmp ? tmp : "/tmp");
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "itest: mkdtemp(%s) failed\n", dir);
        return 2;
    }
    snprintf(sock, sizeof sock, "%s/stored.sock", dir);

    test_two_clients_clean_stop();
    test_bad_input_dropped();
    test_kill9_torn_tail();
    test_client_through_restart();
    test_client_reconnect_rate();
    test_client_never_blocks();
    test_refusals();
    test_periodic_sync();

    if (check_failures == 0) {
        for (int i = 0; i < nnames; i++)
            unlink(names[i]);
        unlink(sock);
        rmdir(dir);
    } else {
        fprintf(stderr, "itest: files kept in %s\n", dir);
    }
    printf("itest_stored: %d checks, %d failed\n", check_count, check_failures);
    return check_failures == 0 ? 0 : 1;
}
