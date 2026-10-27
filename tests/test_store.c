#include "check.h"
#include "suites.h"
#include "common/crc32.h"
#include "common/record.h"
#include "store/store.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    HDR = sizeof(struct dsm_store_header),
    REC = sizeof(struct dsm_record),
    MAX_RECS = 16,
};

/* Every test uses one file inside a fresh temporary directory. */
static char dir[256];
static char path[300];

/* Size of the test file in bytes, or -1 if it does not exist. */
static long file_size(void)
{
    struct stat sb;
    return stat(path, &sb) == 0 ? (long)sb.st_size : -1;
}

/* Replace the test file with exactly these bytes. */
static void write_raw(const void *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "could not create %s", path);
    if (f == NULL)
        return;
    CHECK(fwrite(buf, 1, len, f) == len, "short write to %s", path);
    fclose(f);
}

/* Read up to cap bytes of the test file. Returns how many were read. */
static size_t read_raw(void *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

/* A sealed record whose seq and value make its position recognizable. */
static struct dsm_record make_rec(uint32_t seq)
{
    struct dsm_record r;
    record_init(&r, DSM_KIND_SAMPLE, 3);
    r.seq = seq;
    r.v[0] = (int32_t)seq * 100;
    record_seal(&r);
    return r;
}

/* Append records first..last-1. Returns how many appends failed. */
static int append_range(struct dsm_store *s, uint32_t first, uint32_t last)
{
    int failed = 0;
    for (uint32_t i = first; i < last; i++) {
        struct dsm_record r = make_rec(i);
        if (store_append(s, &r) != STORE_OK)
            failed++;
    }
    return failed;
}

/* Read the file back: it must hold records 0..n-1, in order, all intact. */
static void check_records(uint32_t n)
{
    unsigned char buf[HDR + MAX_RECS * REC];
    size_t got = read_raw(buf, sizeof buf);
    CHECK(got == HDR + n * REC, "read %zu bytes, want %u", got, HDR + n * REC);
    if (got != HDR + n * REC)
        return;

    for (uint32_t i = 0; i < n; i++) {
        struct dsm_record r;
        memcpy(&r, buf + HDR + i * REC, sizeof r);
        CHECK(record_intact(&r), "record %u is not intact", i);
        CHECK(r.seq == i, "record %u has seq %u", i, r.seq);
        CHECK(r.v[0] == (int32_t)i * 100, "record %u has v[0] %d", i, r.v[0]);
    }
}

static void test_new_file_gets_header(void)
{
    unlink(path);
    struct dsm_store *s = NULL;
    CHECK(store_open(path, &s) == STORE_OK, "open of a new file failed");
    CHECK(store_close(s) == STORE_OK, "close failed");
    CHECK(file_size() == HDR, "new file is %ld bytes, want %d", file_size(), HDR);

    struct dsm_store_header h;
    memset(&h, 0, sizeof h);
    read_raw(&h, sizeof h);
    CHECK(memcmp(h.magic, DSM_STORE_MAGIC, sizeof h.magic) == 0, "magic is not DSKY");
    CHECK(h.schema_ver == DSM_SCHEMA_VER, "schema_ver %u", h.schema_ver);
    CHECK(h.record_size == REC, "record_size %u, want %d", h.record_size, REC);
    CHECK(h.created_real_ns > 0, "creation time was not filled in");
    CHECK(h.crc32 == dsm_crc32(&h, offsetof(struct dsm_store_header, crc32)),
          "header CRC does not match");
}

static void test_append_and_read_back(void)
{
    unlink(path);
    struct dsm_store *s = NULL;
    if (store_open(path, &s) != STORE_OK) {
        CHECK(0, "open failed");
        return;
    }
    int failed = append_range(s, 0, 10);
    CHECK(failed == 0, "%d of 10 appends failed", failed);
    CHECK(store_close(s) == STORE_OK, "close failed");
    check_records(10);
}

/* Reopening must add to the end, and must not rewrite the header. */
static void test_reopen_appends_at_end(void)
{
    unlink(path);
    struct dsm_store *s = NULL;
    if (store_open(path, &s) != STORE_OK) {
        CHECK(0, "first open failed");
        return;
    }
    CHECK(append_range(s, 0, 3) == 0, "appends before reopen failed");
    store_close(s);

    struct dsm_store_header before, after;
    read_raw(&before, sizeof before);

    if (store_open(path, &s) != STORE_OK) {
        CHECK(0, "reopen failed");
        return;
    }
    CHECK(append_range(s, 3, 5) == 0, "appends after reopen failed");
    store_close(s);

    read_raw(&after, sizeof after);
    CHECK(memcmp(&before, &after, sizeof before) == 0, "reopen rewrote the header");
    check_records(5);
}

/* A file that isn't ours is refused and left exactly as it was. */
static void check_refused(const void *bytes, size_t len, const char *what)
{
    write_raw(bytes, len);
    struct dsm_store *s = NULL;
    CHECK(store_open(path, &s) == STORE_ERR_FORMAT, "%s: not refused", what);
    CHECK(s == NULL, "%s: got a store handle anyway", what);
    store_close(s);

    unsigned char back[HDR + MAX_RECS * REC];
    size_t n = read_raw(back, sizeof back);
    CHECK(n == len && memcmp(back, bytes, len) == 0, "%s: file was modified", what);
}

static void test_foreign_files_refused(void)
{
    const char text[] = "somebody's notes file, definitely not ours\n";
    check_refused(text, sizeof text - 1, "text file");

    const char shorty[] = "DSKY123456";
    check_refused(shorty, sizeof shorty - 1, "file shorter than a header");

    /* A real store file with one byte of its header damaged. */
    unlink(path);
    struct dsm_store *s = NULL;
    if (store_open(path, &s) != STORE_OK) {
        CHECK(0, "open failed");
        return;
    }
    append_range(s, 0, 2);
    store_close(s);
    unsigned char buf[HDR + 2 * REC];
    size_t n = read_raw(buf, sizeof buf);
    CHECK(n == sizeof buf, "store file is %zu bytes, want %zu", n, sizeof buf);
    buf[10] ^= 0xFF;   /* inside created_real_ns */
    check_refused(buf, n, "damaged header");
}

/* A header that matches what store_open() writes, with its CRC sealed. */
static void good_header(struct dsm_store_header *h)
{
    memset(h, 0, sizeof *h);
    memcpy(h->magic, DSM_STORE_MAGIC, sizeof h->magic);
    h->schema_ver = DSM_SCHEMA_VER;
    h->record_size = REC;
    h->created_real_ns = 1761436800000000000ull;
    h->crc32 = dsm_crc32(h, offsetof(struct dsm_store_header, crc32));
}

/* The CRC only proves the header wasn't damaged. A header can be intact
 * and still wrong for us: a different kind of file, or one written by a
 * newer version with a different record format. Each field is checked. */
static void test_intact_but_wrong_header_refused(void)
{
    const size_t crc_at = offsetof(struct dsm_store_header, crc32);
    struct dsm_store_header h;

    good_header(&h);
    write_raw(&h, sizeof h);
    struct dsm_store *s = NULL;
    CHECK(store_open(path, &s) == STORE_OK, "a correct header was refused");
    store_close(s);

    good_header(&h);
    h.magic[3] = 'X';
    h.crc32 = dsm_crc32(&h, crc_at);
    check_refused(&h, sizeof h, "different magic");

    good_header(&h);
    h.schema_ver = DSM_SCHEMA_VER + 1;
    h.crc32 = dsm_crc32(&h, crc_at);
    check_refused(&h, sizeof h, "newer schema version");

    good_header(&h);
    h.record_size = REC + 16;
    h.crc32 = dsm_crc32(&h, crc_at);
    check_refused(&h, sizeof h, "different record size");
}

/* A record whose CRC doesn't match must never reach the file. Later,
 * recovery cuts the file off at the first bad record, so one bad record
 * written now would throw away every good record written after it. */
static void test_bad_record_refused(void)
{
    unlink(path);
    struct dsm_store *s = NULL;
    if (store_open(path, &s) != STORE_OK) {
        CHECK(0, "open failed");
        return;
    }
    struct dsm_record r = make_rec(0);
    r.v[1] = 5;   /* changed after sealing */
    CHECK(store_append(s, &r) == STORE_ERR_BADREC, "unsealed record accepted");
    store_close(s);
    CHECK(file_size() == HDR, "file is %ld bytes; the bad record got in", file_size());
}

/* Make a write really fail: lower the largest file size this process is
 * allowed to write (RLIMIT_FSIZE) so the third record doesn't fit. After
 * that, we don't know where the file ends, so every later append must be
 * refused, even once the limit is lifted again. */
static void test_failed_write_stops_appends(void)
{
    unlink(path);
    struct dsm_store *s = NULL;
    if (store_open(path, &s) != STORE_OK) {
        CHECK(0, "open failed");
        return;
    }

    struct rlimit old, low;
    if (getrlimit(RLIMIT_FSIZE, &old) != 0) {
        CHECK(0, "getrlimit failed");
        store_close(s);
        return;
    }
    low = old;
    low.rlim_cur = HDR + 2 * REC + 10;   /* room for two records and 10 bytes */

    /* Without this, going over the limit kills the process with SIGXFSZ
     * instead of returning an error from write(). */
    void (*old_handler)(int) = signal(SIGXFSZ, SIG_IGN);
    int lowered = setrlimit(RLIMIT_FSIZE, &low) == 0;

    struct dsm_record r0 = make_rec(0), r1 = make_rec(1), r2 = make_rec(2);
    enum store_status a = store_append(s, &r0);
    enum store_status b = store_append(s, &r1);
    enum store_status c = store_append(s, &r2);

    setrlimit(RLIMIT_FSIZE, &old);
    signal(SIGXFSZ, old_handler);

    struct dsm_record r3 = make_rec(3);
    enum store_status d = store_append(s, &r3);
    store_close(s);

    CHECK(lowered, "setrlimit failed, so this test proved nothing");
    CHECK(a == STORE_OK && b == STORE_OK, "appends under the limit failed: %d %d", a, b);
    CHECK(c == STORE_ERR_IO, "append over the limit returned %d, want STORE_ERR_IO", c);
    CHECK(d == STORE_ERR_BROKEN, "append after a failure returned %d, want STORE_ERR_BROKEN", d);
    long size = file_size();
    CHECK(size >= HDR + 2 * REC && size <= HDR + 2 * REC + 10,
          "file is %ld bytes, want %d..%d", size, HDR + 2 * REC, HDR + 2 * REC + 10);
}

void test_store(void)
{
    /* Not mkdtemp(): macOS hides it under -D_POSIX_C_SOURCE. The process
     * ID makes the name unique for as long as this test runs. */
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/dsm-test-%ld",
             tmp != NULL && *tmp ? tmp : "/tmp", (long)getpid());
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        CHECK(0, "mkdir(%s) failed", dir);
        return;
    }
    snprintf(path, sizeof path, "%s/store.dsm", dir);

    test_new_file_gets_header();
    test_append_and_read_back();
    test_reopen_appends_at_end();
    test_foreign_files_refused();
    test_intact_but_wrong_header_refused();
    test_bad_record_refused();
    test_failed_write_stops_appends();

    unlink(path);
    rmdir(dir);
}
