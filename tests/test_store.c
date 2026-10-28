#include "check.h"
#include "suites.h"
#include "common/crc32.h"
#include "common/record.h"
#include "store/store.h"

#include <errno.h>
#include <inttypes.h>
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

/* Add bytes to the end of the test file, the way a crash in the middle of
 * a write leaves them. */
static void append_raw(const void *buf, size_t len)
{
    FILE *f = fopen(path, "ab");
    CHECK(f != NULL, "could not open %s", path);
    if (f == NULL)
        return;
    CHECK(fwrite(buf, 1, len, f) == len, "short append to %s", path);
    fclose(f);
}

/* A fresh store file holding records 0..n-1. */
static void make_store(uint32_t n)
{
    unlink(path);
    struct dsm_store *s = NULL;
    if (store_open(path, &s) != STORE_OK) {
        CHECK(0, "open failed");
        return;
    }
    struct store_recovery got = store_get_recovery(s);
    CHECK(got.kept == 0 && got.cut_bytes == 0, "a new file reports recovery");
    CHECK(append_range(s, 0, n) == 0, "appends failed");
    store_close(s);
}

/* Reopen the test file, which runs recovery, and say what it did. */
static struct store_recovery reopen(void)
{
    struct store_recovery got = {0, 0};
    struct dsm_store *s = NULL;
    CHECK(store_open(path, &s) == STORE_OK, "reopen failed");
    if (s != NULL)
        got = store_get_recovery(s);
    store_close(s);
    return got;
}

/* A power cut in the middle of a write leaves part of a record at the end.
 * Reopening must cut exactly those bytes, keep every whole record, and put
 * the next append back on a record boundary. */
static void test_recover_torn_tail(void)
{
    make_store(10);
    unsigned char junk[20];
    memset(junk, 0xAB, sizeof junk);
    append_raw(junk, sizeof junk);

    struct store_recovery got = reopen();
    CHECK(got.kept == 10, "kept %" PRIu64 ", want 10", got.kept);
    CHECK(got.cut_bytes == 20, "cut %" PRIu64 " bytes, want 20", got.cut_bytes);
    CHECK(file_size() == HDR + 10 * REC, "file is %ld bytes", file_size());
    check_records(10);

    struct dsm_store *s = NULL;
    if (store_open(path, &s) != STORE_OK) {
        CHECK(0, "open after recovery failed");
        return;
    }
    CHECK(append_range(s, 10, 12) == 0, "appends after recovery failed");
    store_close(s);
    check_records(12);
}

/* One damaged byte in record 5 of 10. Records 6-9 still pass their own
 * CRCs, but they are cut too. The store never writes a bad record, so a
 * bad one in the middle means the file was damaged after it was written,
 * and we can't tell how far the damage goes. The rule stays simple: keep
 * everything before the first bad record, nothing after it. */
static void test_recover_corrupt_middle(void)
{
    make_store(10);
    unsigned char buf[HDR + 10 * REC];
    size_t n = read_raw(buf, sizeof buf);
    CHECK(n == sizeof buf, "store file is %zu bytes, want %zu", n, sizeof buf);
    buf[HDR + 5 * REC + 30] ^= 0x01;   /* inside record 5's values */
    write_raw(buf, n);

    struct store_recovery got = reopen();
    CHECK(got.kept == 5, "kept %" PRIu64 ", want 5", got.kept);
    CHECK(got.cut_bytes == 5 * REC, "cut %" PRIu64 " bytes, want %d",
          got.cut_bytes, 5 * REC);
    check_records(5);
}

/* After a power cut, a file can end in a block of zeros: the file's size
 * was saved, the data never was. Zeros must never pass as a record. */
static void test_recover_zero_slot(void)
{
    make_store(3);
    unsigned char zeros[REC];
    memset(zeros, 0, sizeof zeros);
    append_raw(zeros, sizeof zeros);

    struct store_recovery got = reopen();
    CHECK(got.kept == 3, "kept %" PRIu64 ", want 3", got.kept);
    CHECK(got.cut_bytes == REC, "cut %" PRIu64 " bytes, want %d", got.cut_bytes, REC);
    check_records(3);
}

/* A clean file must come through recovery byte for byte unchanged. */
static void test_recover_clean_file_untouched(void)
{
    make_store(4);
    unsigned char before[HDR + 4 * REC], after[HDR + 4 * REC];
    size_t nb = read_raw(before, sizeof before);

    struct store_recovery got = reopen();
    CHECK(got.kept == 4, "kept %" PRIu64 ", want 4", got.kept);
    CHECK(got.cut_bytes == 0, "cut %" PRIu64 " bytes from a clean file", got.cut_bytes);
    size_t na = read_raw(after, sizeof after);
    CHECK(na == nb && memcmp(before, after, nb) == 0, "recovery changed a clean file");
}

/* A header with no records, then a header with less than one record. */
static void test_recover_header_only(void)
{
    make_store(0);
    struct store_recovery got = reopen();
    CHECK(got.kept == 0 && got.cut_bytes == 0, "header-only file: kept %" PRIu64
          ", cut %" PRIu64, got.kept, got.cut_bytes);

    unsigned char junk[30];
    memset(junk, 0x5A, sizeof junk);
    append_raw(junk, sizeof junk);
    got = reopen();
    CHECK(got.kept == 0, "kept %" PRIu64 ", want 0", got.kept);
    CHECK(got.cut_bytes == 30, "cut %" PRIu64 " bytes, want 30", got.cut_bytes);
    CHECK(file_size() == HDR, "file is %ld bytes, want %d", file_size(), HDR);
}

/* Running recovery twice does the same as running it once (lec11: undo and
 * redo are idempotent, because a crash can also happen during recovery). */
static void test_recover_idempotent(void)
{
    make_store(6);
    unsigned char junk[7];
    memset(junk, 0xEE, sizeof junk);
    append_raw(junk, sizeof junk);

    struct store_recovery first = reopen();
    unsigned char once[HDR + 6 * REC + 8], twice[HDR + 6 * REC + 8];
    size_t n1 = read_raw(once, sizeof once);

    struct store_recovery second = reopen();
    size_t n2 = read_raw(twice, sizeof twice);

    CHECK(first.kept == 6 && first.cut_bytes == 7, "first recovery: kept %" PRIu64
          ", cut %" PRIu64, first.kept, first.cut_bytes);
    CHECK(second.kept == 6 && second.cut_bytes == 0, "second recovery: kept %" PRIu64
          ", cut %" PRIu64, second.kept, second.cut_bytes);
    CHECK(n1 == n2 && memcmp(once, twice, n1) == 0, "second recovery changed the file");
}

/* Counters say what this handle did, so stored can log them and a test can
 * prove how many records, writes and fsyncs a sequence of calls cost. */
static void test_counters(void)
{
    unlink(path);
    struct dsm_store *s = NULL;
    if (store_open(path, &s) != STORE_OK) {
        CHECK(0, "open failed");
        return;
    }
    struct store_counters c = store_get_counters(s);
    CHECK(c.records == 0 && c.writes == 0 && c.fsyncs == 0,
          "new handle: records %" PRIu64 " writes %" PRIu64 " fsyncs %" PRIu64,
          c.records, c.writes, c.fsyncs);

    CHECK(append_range(s, 0, 3) == 0, "appends failed");
    c = store_get_counters(s);
    CHECK(c.records == 3, "records %" PRIu64 ", want 3", c.records);
    CHECK(c.writes == 3, "writes %" PRIu64 ", want 3 (one per append)", c.writes);
    CHECK(c.fsyncs == 0, "fsyncs %" PRIu64 " before any store_sync", c.fsyncs);

    struct dsm_record bad = make_rec(3);
    bad.v[1] = 5;   /* changed after sealing */
    CHECK(store_append(s, &bad) == STORE_ERR_BADREC, "unsealed record accepted");
    c = store_get_counters(s);
    CHECK(c.records == 3 && c.writes == 3, "a refused record was counted");

    CHECK(store_sync(s) == STORE_OK, "first sync failed");
    CHECK(store_sync(s) == STORE_OK, "second sync failed");
    c = store_get_counters(s);
    CHECK(c.fsyncs == 2, "fsyncs %" PRIu64 ", want 2", c.fsyncs);
    CHECK(c.records == 3, "a sync changed records to %" PRIu64, c.records);
    CHECK(store_close(s) == STORE_OK, "close failed");
    check_records(3);

    /* A reopened file already holds 3 records, but this handle wrote none. */
    if (store_open(path, &s) != STORE_OK) {
        CHECK(0, "reopen failed");
        return;
    }
    c = store_get_counters(s);
    CHECK(c.records == 0 && c.writes == 0 && c.fsyncs == 0,
          "reopened handle starts at records %" PRIu64 " writes %" PRIu64
          " fsyncs %" PRIu64, c.records, c.writes, c.fsyncs);
    store_close(s);
}

/* Make the next append fail for real, as in test_failed_write_stops_appends:
 * lower RLIMIT_FSIZE to 10 bytes past the current end. Returns 1 if the
 * handle is now broken. */
static int break_store(struct dsm_store *s)
{
    struct rlimit old, low;
    if (getrlimit(RLIMIT_FSIZE, &old) != 0)
        return 0;
    low = old;
    low.rlim_cur = (rlim_t)file_size() + 10;

    void (*old_handler)(int) = signal(SIGXFSZ, SIG_IGN);
    int lowered = setrlimit(RLIMIT_FSIZE, &low) == 0;
    struct dsm_record r = make_rec(99);
    enum store_status st = store_append(s, &r);
    setrlimit(RLIMIT_FSIZE, &old);
    signal(SIGXFSZ, old_handler);

    return lowered && st == STORE_ERR_IO;
}

/* After a failed write we don't know where the file ends, so a sync must
 * not report success: the caller would believe its data was safe. Close
 * still frees the handle (valgrind and ASan check that), and the next open
 * cuts off whatever part of the failed record reached the file. */
static void test_sync_on_broken_handle(void)
{
    unlink(path);
    struct dsm_store *s = NULL;
    if (store_open(path, &s) != STORE_OK) {
        CHECK(0, "open failed");
        return;
    }
    CHECK(append_range(s, 0, 1) == 0, "append failed");
    CHECK(break_store(s), "could not make a write fail, so this test proved nothing");

    CHECK(store_sync(s) == STORE_ERR_BROKEN, "sync on a broken handle did not return BROKEN");
    struct store_counters c = store_get_counters(s);
    CHECK(c.fsyncs == 0, "a refused sync was counted");
    CHECK(c.records == 1, "records %" PRIu64 ", want 1 (the failed one is not counted)",
          c.records);
    CHECK(store_close(s) == STORE_ERR_BROKEN, "close of a broken handle did not return BROKEN");

    struct store_recovery got = reopen();
    CHECK(got.kept == 1, "kept %" PRIu64 ", want 1", got.kept);
    CHECK(got.cut_bytes <= 10, "cut %" PRIu64 " bytes, want 0..10", got.cut_bytes);
    check_records(1);
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
    test_recover_torn_tail();
    test_recover_corrupt_middle();
    test_recover_zero_slot();
    test_recover_clean_file_untouched();
    test_recover_header_only();
    test_recover_idempotent();
    test_counters();
    test_sync_on_broken_handle();

    unlink(path);
    rmdir(dir);
}
