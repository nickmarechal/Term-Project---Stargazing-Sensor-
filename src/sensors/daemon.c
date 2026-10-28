#include "sensors/daemon.h"
#include "sensors/i2c.h"
#include "common/log.h"
#include "store/store_client.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define I2C_DEV       "/dev/i2c-1"
#define MAX_FAILS     3   /* this many failed reads in a row = sensor absent */
#define EXIT_ABSENT   3   /* for dskyd (M4): park it, re-probe later */

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static uint64_t now_ns(clockid_t clk)
{
    struct timespec ts;
    if (clock_gettime(clk, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* Every record goes out the same way: times, seq, seal (last), send. */
struct run {
    const struct sensor_ops *ops;
    struct store_client *client;
    uint32_t seq;
    bool stored_up;
};

static void emit(struct run *run, struct dsm_record *r)
{
    r->t_mono_ns = now_ns(CLOCK_MONOTONIC);
    r->t_real_ns = now_ns(CLOCK_REALTIME);
    r->seq = run->seq++;
    record_seal(r);

    bool sent = store_client_send(run->client, r) == 0;
    if (sent != run->stored_up) {   /* log only when it changes */
        if (sent)
            log_msg(LVL_INFO, "stored_up", NULL);
        else
            log_msg(LVL_WARN, "stored_down", "dropped=%llu",
                    (unsigned long long)store_client_dropped(run->client));
        run->stored_up = sent;
    }
}

static void emit_fault(struct run *run, int code, int err, int count)
{
    struct dsm_record r;
    record_init(&r, DSM_KIND_FAULT, run->ops->source);
    r.v[0] = code;
    r.v[1] = err;
    r.v[2] = count;
    emit(run, &r);
}

int sensor_main(const struct sensor_ops *ops, int argc, char **argv)
{
    log_init(ops->name, stderr);
    if (argc < 2 || argc > 3) {
        log_msg(LVL_ERROR, "usage", "args=\"<stored-socket> [run]\"");
        return 1;
    }
    int run_no = argc == 3 ? atoi(argv[2]) : 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;           /* no SA_RESTART: sleeps wake up */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    struct run run = { ops, store_client_new(argv[1]), 0, true };
    if (run.client == NULL) {
        log_msg(LVL_ERROR, "client_fail", "errno=%d", errno);
        return 1;
    }

    int fd = i2c_open(I2C_DEV);
    if (fd < 0) {
        log_msg(LVL_ERROR, "open_fail", "dev=%s errno=%d", I2C_DEV, errno);
        store_client_free(run.client);
        return 1;
    }

    struct dsm_record r;
    record_init(&r, DSM_KIND_START, ops->source);
    r.v[0] = run_no;
    emit(&run, &r);

    uint8_t id = 0;
    int st = ops->setup(fd, &id);
    if (st != SETUP_OK) {
        int err = st == SETUP_IO ? errno : 0;
        emit_fault(&run, st == SETUP_IO ? DSM_FAULT_READ : DSM_FAULT_CHIP_ID, err, 1);
        log_msg(LVL_ERROR, "setup_fail", "id=0x%02x errno=%d", id, err);
        close(fd);
        store_client_free(run.client);
        return EXIT_ABSENT;
    }
    log_msg(LVL_INFO, "start", "run=%d period_ms=%d", run_no, ops->period_ms);

    /* Absolute deadlines, so the period doesn't drift by however long each
     * read took. */
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    int fails = 0, rc = 0;
    while (!stop) {
        next.tv_nsec += (long)(ops->period_ms % 1000) * 1000000L;
        next.tv_sec += ops->period_ms / 1000 + next.tv_nsec / 1000000000L;
        next.tv_nsec %= 1000000000L;
        if (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) != 0)
            continue;            /* a signal: the loop test sees stop */

        record_init(&r, DSM_KIND_SAMPLE, ops->source);
        if (ops->read(fd, &r) == 0) {
            fails = 0;
            emit(&run, &r);
            continue;
        }
        int err = errno;
        fails++;
        emit_fault(&run, DSM_FAULT_READ, err, fails);
        log_msg(LVL_WARN, "read_fail", "errno=%d n=%d", err, fails);
        if (fails >= MAX_FAILS) {
            log_msg(LVL_ERROR, "absent", "n=%d", fails);
            rc = EXIT_ABSENT;
            break;
        }
    }

    log_msg(LVL_INFO, "stop", "records=%u dropped=%llu", run.seq,
            (unsigned long long)store_client_dropped(run.client));
    close(fd);
    store_client_free(run.client);
    return rc;
}
