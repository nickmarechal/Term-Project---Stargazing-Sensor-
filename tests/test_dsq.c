#include "check.h"
#include "suites.h"
#include "common/record.h"
#include "dsq/dump.h"
#include "store/store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* 2026-10-29 02:03:04.567 UTC */
#define T_REAL 1793239384567000000ull

static struct dsm_record rec(uint16_t src, uint16_t kind, uint16_t flags, uint32_t seq,
                             int32_t a, int32_t b, int32_t c, int32_t d)
{
    struct dsm_record r;
    record_init(&r, (enum dsm_kind)kind, src);
    r.t_real_ns = T_REAL;
    r.flags = flags;
    r.seq = seq;
    r.v[0] = a;
    r.v[1] = b;
    r.v[2] = c;
    r.v[3] = d;
    record_seal(&r);
    return r;
}

static void expect_line(const struct dsm_record *r, const char *want)
{
    char line[256];
    dump_format_record(line, sizeof line, r);
    CHECK(strcmp(line, want) == 0, "\n    got  \"%s\"\n    want \"%s\"", line, want);
}

/* Every payload in the table, in its units. Times are checked in UTC so
 * the expected strings don't depend on where the test runs. */
static void test_record_lines(void)
{
    struct dsm_record r;

    r = rec(DSM_SRC_SKYD, DSM_KIND_SAMPLE, DSM_FLAG_VALID, 7, 1234, 56, 428, 600);
    expect_line(&r, "2026-10-29T02:03:04.567+0000 skyd    SAMPLE    seq=7 flags=VALID"
                    " ch0=1234 ch1=56 gain=428x atime=600ms");

    r = rec(DSM_SRC_CLOUDD, DSM_KIND_SAMPLE, DSM_FLAG_VALID | DSM_FLAG_SYNTHETIC, 12,
            -18240, 4120, 0, 0);
    expect_line(&r, "2026-10-29T02:03:04.567+0000 cloudd  SAMPLE    seq=12 flags=VALID,SYNTHETIC"
                    " sky=-18.240C sensor=4.120C");

    /* Between -1 and 0 the sign must not get lost. */
    r = rec(DSM_SRC_ENVD, DSM_KIND_SAMPLE, 0, 3, -500, 45200, 84123, 0);
    expect_line(&r, "2026-10-29T02:03:04.567+0000 envd    SAMPLE    seq=3 flags=-"
                    " temp=-0.500C rh=45.200% pressure=84123Pa");

    r = rec(DSM_SRC_CLOUDD, DSM_KIND_FAULT, 0, 4, DSM_FAULT_READ, 121, 3, 0);
    expect_line(&r, "2026-10-29T02:03:04.567+0000 cloudd  FAULT     seq=4 flags=-"
                    " fault=read errno=121 count=3");

    r = rec(DSM_SRC_SKYD, DSM_KIND_FAULT, 0, 1, DSM_FAULT_CHIP_ID, 0, 1, 0);
    expect_line(&r, "2026-10-29T02:03:04.567+0000 skyd    FAULT     seq=1 flags=-"
                    " fault=chip_id errno=0 count=1");

    r = rec(DSM_SRC_STORED, DSM_KIND_START, 0, 0, 0, 0, 0, 0);
    expect_line(&r, "2026-10-29T02:03:04.567+0000 stored  START     seq=0 flags=- run=0");

    r = rec(DSM_SRC_DSKYD, DSM_KIND_HEARTBEAT, 0, 9, DSM_SRC_CLOUDD, 1824, 350, 86400);
    expect_line(&r, "2026-10-29T02:03:04.567+0000 dskyd   HEARTBEAT seq=9 flags=-"
                    " subject=cloudd rss=1824KiB cpu=350ms events=86400");

    r = rec(DSM_SRC_FUSIOND, DSM_KIND_FEATURE, DSM_FLAG_VALID, 2, 3, 21500, 900, 180);
    expect_line(&r, "2026-10-29T02:03:04.567+0000 fusiond FEATURE   seq=2 flags=VALID"
                    " feature=3 value=21.500 window=900s n=180");

    r = rec(DSM_SRC_FUSIOND, DSM_KIND_STATE, DSM_FLAG_STALE | DSM_FLAG_CALIBRATING, 5,
            1, 0x15, 850, 0);
    expect_line(&r, "2026-10-29T02:03:04.567+0000 fusiond STATE     seq=5 flags=STALE,CALIBRATING"
                    " state=1 rules=0x15 confidence=0.850");

    /* Values whose meaning isn't known are shown raw, never dropped. */
    r = rec(42, 9, 0x80, 1, 1, -2, 3, -4);
    expect_line(&r, "2026-10-29T02:03:04.567+0000 src42   kind9     seq=1 flags=0x80"
                    " v=1,-2,3,-4");

    r = rec(DSM_SRC_SKYD, DSM_KIND_SAMPLE, 0, 0, 0, 0, 0, 0);
    r.t_real_ns = 0;
    record_seal(&r);
    expect_line(&r, "time-unknown skyd    SAMPLE    seq=0 flags=- ch0=0 ch1=0 gain=0x atime=0ms");

    /* A line cut short still ends in a '\0' inside the buffer. */
    char small[16];
    r = rec(DSM_SRC_SKYD, DSM_KIND_SAMPLE, 0, 0, 0, 0, 0, 0);
    int want = dump_format_record(small, sizeof small, &r);
    CHECK(want > (int)sizeof small && strlen(small) == sizeof small - 1,
          "truncation: wanted %d, wrote %zu", want, strlen(small));
}

static void test_header_and_summary(void)
{
    char line[256];
    struct dsm_store_header h;
    memset(&h, 0, sizeof h);
    h.schema_ver = DSM_SCHEMA_VER;
    h.record_size = sizeof(struct dsm_record);
    h.created_real_ns = T_REAL;
    dump_format_header(line, sizeof line, &h);
    CHECK(strcmp(line, "header: schema=1 record_size=48 created=2026-10-29T02:03:04.567+0000") == 0,
          "header line \"%s\"", line);

    dump_format_summary(line, sizeof line, 118, 31, 0);
    CHECK(strcmp(line, "118 records; torn tail: 31 bytes (not modified)") == 0,
          "summary \"%s\"", line);
    dump_format_summary(line, sizeof line, 5, 0, 5);
    CHECK(strcmp(line, "5 records; torn tail: 0 bytes (not modified); 5 SYNTHETIC") == 0,
          "summary \"%s\"", line);
}

/* Read all of f back into buf as a string. */
static void slurp(FILE *f, char *buf, size_t cap)
{
    rewind(f);
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
}

static int count_lines(const char *s)
{
    int n = 0;
    for (; *s; s++)
        n += *s == '\n';
    return n;
}

/* The whole command on a real file with a torn tail, all records, then
 * the last 2. The file must not change. */
static void test_dump_file(const char *dir)
{
    char path[300];
    snprintf(path, sizeof path, "%s/dump.dsm", dir);
    unlink(path);

    struct dsm_store *s = NULL;
    if (store_open(path, &s) != STORE_OK) {
        CHECK(0, "open failed");
        return;
    }
    for (uint32_t i = 0; i < 4; i++) {
        struct dsm_record r = rec(DSM_SRC_CLOUDD, DSM_KIND_SAMPLE,
                                  DSM_FLAG_VALID | DSM_FLAG_SYNTHETIC, i, 1000 * (int32_t)i, 0, 0, 0);
        store_append(s, &r);
    }
    store_close(s);
    FILE *f = fopen(path, "ab");
    if (f != NULL) {
        fwrite("0123456789", 1, 10, f);
        fclose(f);
    }
    struct stat before, after;
    stat(path, &before);

    static char text[8192];
    FILE *out = tmpfile(), *err = tmpfile();
    CHECK(out != NULL && err != NULL, "tmpfile failed");
    if (out == NULL || err == NULL)
        return;

    CHECK(dump_file(path, 0, out, err) == 0, "dump failed");
    slurp(out, text, sizeof text);
    CHECK(strncmp(text, "header: schema=1 record_size=48 created=", 40) == 0,
          "output does not start with the header:\n%s", text);
    CHECK(count_lines(text) == 6, "%d lines, want header + 4 records + summary:\n%s",
          count_lines(text), text);
    CHECK(strstr(text, "seq=0 flags=VALID,SYNTHETIC sky=0.000C") != NULL, "record 0 missing");
    CHECK(strstr(text, "seq=3 flags=VALID,SYNTHETIC sky=3.000C") != NULL, "record 3 missing");
    CHECK(strstr(text, "\n4 records; torn tail: 10 bytes (not modified); 4 SYNTHETIC\n") != NULL,
          "summary wrong:\n%s", text);

    fclose(out);
    out = tmpfile();
    CHECK(dump_file(path, 2, out, err) == 0, "dump -n 2 failed");
    slurp(out, text, sizeof text);
    CHECK(count_lines(text) == 4, "-n 2: %d lines, want header + 2 + summary:\n%s",
          count_lines(text), text);
    CHECK(strstr(text, "seq=1 ") == NULL && strstr(text, "seq=2 ") != NULL
          && strstr(text, "seq=3 ") != NULL, "-n 2 did not show the last two:\n%s", text);
    CHECK(strstr(text, "\n4 records;") != NULL, "-n 2: the summary must still count all 4");

    stat(path, &after);
    CHECK(after.st_size == before.st_size, "dump changed the file's size");

    /* A file that isn't ours: exit 1, a reason on err, nothing on out. */
    fclose(out);
    out = tmpfile();
    f = fopen(path, "wb");
    if (f != NULL) {
        fputs("not a store file at all, just some text\n", f);
        fclose(f);
    }
    CHECK(dump_file(path, 0, out, err) == 1, "dump of a foreign file did not fail");
    slurp(out, text, sizeof text);
    CHECK(text[0] == '\0', "dump of a foreign file printed:\n%s", text);
    slurp(err, text, sizeof text);
    CHECK(strstr(text, "not a store file") != NULL, "no reason given:\n%s", text);

    fclose(out);
    fclose(err);
    unlink(path);
}

void test_dsq(void)
{
    /* Expected strings are in UTC. Put the caller's zone back afterwards. */
    const char *old = getenv("TZ");
    char saved[128] = "";
    if (old != NULL)
        snprintf(saved, sizeof saved, "%s", old);
    setenv("TZ", "UTC0", 1);
    tzset();

    test_record_lines();
    test_header_and_summary();

    const char *tmp = getenv("TMPDIR");
    char dir[256];
    snprintf(dir, sizeof dir, "%s/dsm-dsq-%ld", tmp != NULL && *tmp ? tmp : "/tmp", (long)getpid());
    if (mkdir(dir, 0700) == 0 || errno == EEXIST) {
        test_dump_file(dir);
        rmdir(dir);
    } else {
        CHECK(0, "mkdir(%s) failed", dir);
    }

    if (old != NULL)
        setenv("TZ", saved, 1);
    else
        unsetenv("TZ");
    tzset();
}
