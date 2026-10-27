#include "check.h"
#include "suites.h"
#include "common/record.h"

#include <string.h>

/* Fill with 0xAA first, so record_init has to actually overwrite garbage. */
static void fresh(struct dsm_record *r)
{
    memset(r, 0xAA, sizeof *r);
    record_init(r, DSM_KIND_SAMPLE, 7);
}

static void test_init(void)
{
    struct dsm_record r;
    fresh(&r);
    CHECK(r.schema_ver == DSM_SCHEMA_VER, "schema_ver %u, want %d",
          r.schema_ver, DSM_SCHEMA_VER);
    CHECK(r.kind == DSM_KIND_SAMPLE, "kind %u, want %d", r.kind, DSM_KIND_SAMPLE);
    CHECK(r.source_id == 7, "source_id %u, want 7", r.source_id);
    CHECK(r.flags == 0 && r.seq == 0 && r.crc32 == 0,
          "flags/seq/crc32 not zeroed: %u %u %u", r.flags, r.seq, r.crc32);
    CHECK(r.v[0] == 0 && r.v[1] == 0 && r.v[2] == 0 && r.v[3] == 0,
          "v[] not zeroed");
}

static void test_sealed_is_intact(void)
{
    struct dsm_record r;
    fresh(&r);
    r.v[0] = 12345;
    r.seq = 42;
    record_seal(&r);
    CHECK(record_intact(&r), "a freshly sealed record is not intact");
}

/* The core property: damage to ANY byte is caught, including the four
 * bytes of the CRC itself. CRC-32 is guaranteed to catch any damage that
 * fits inside 32 bits in a row, so one bad byte is always caught. */
static void test_every_byte_flip_is_caught(void)
{
    struct dsm_record r;
    fresh(&r);
    r.t_real_ns = 1761436800000000000ull;
    r.v[2] = -900;
    record_seal(&r);

    unsigned char *bytes = (unsigned char *)&r;
    int missed = 0;
    for (size_t i = 0; i < sizeof r; i++) {
        bytes[i] ^= 0xFF;
        if (record_intact(&r))
            missed++;
        bytes[i] ^= 0xFF;
    }
    CHECK(missed == 0, "%d of %zu byte flips went undetected", missed, sizeof r);
    CHECK(record_intact(&r), "record not intact after undoing every flip");
}

/* A power cut can leave a stretch of zeros at the end of a file. That
 * must never be read back as a real record. */
static void test_all_zeros_is_not_intact(void)
{
    struct dsm_record r;
    memset(&r, 0, sizeof r);
    CHECK(!record_intact(&r), "an all-zero record passed as intact");
}

/* Seal last. Editing any field afterward breaks the record until resealed. */
static void test_edit_after_seal(void)
{
    struct dsm_record r;
    fresh(&r);
    record_seal(&r);
    r.seq = 99;
    CHECK(!record_intact(&r), "editing seq after sealing went undetected");
    record_seal(&r);
    CHECK(record_intact(&r), "resealing did not make it intact again");
}

void test_record(void)
{
    test_init();
    test_sealed_is_intact();
    test_every_byte_flip_is_caught();
    test_all_zeros_is_not_intact();
    test_edit_after_seal();
}
