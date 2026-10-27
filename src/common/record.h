#ifndef DSM_RECORD_H
#define DSM_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DSM_SCHEMA_VER 1

/* Starts at 1 so a zeroed record never looks like a real kind. */
enum dsm_kind {
    DSM_KIND_SAMPLE    = 1,
    DSM_KIND_FEATURE   = 2,
    DSM_KIND_STATE     = 3,
    DSM_KIND_HEARTBEAT = 4,
    DSM_KIND_FAULT     = 5,
};

/* Bits in dsm_record.flags. They describe the reading, not the bytes.
 * SYNTHETIC is required by the spec: replayed or made-up data has to be
 * labeled everywhere it appears, so it is part of the record itself. */
enum dsm_flag {
    DSM_FLAG_VALID       = 1u << 0,
    DSM_FLAG_STALE       = 1u << 1,
    DSM_FLAG_SYNTHETIC   = 1u << 2,
    DSM_FLAG_CALIBRATING = 1u << 3,
};

/* One record, exactly as it sits on disk. */
struct dsm_record {
    uint64_t t_mono_ns;   /* CLOCK_MONOTONIC: for measuring intervals */
    uint64_t t_real_ns;   /* CLOCK_REALTIME: the wall-clock label */
    uint16_t schema_ver;
    uint16_t source_id;   /* which process produced it */
    uint16_t kind;        /* enum dsm_kind */
    uint16_t flags;       /* enum dsm_flag bits */
    int32_t  v[4];        /* fixed-point values; meaning depends on kind */
    uint32_t seq;         /* per-source counter: a gap means lost data */
    uint32_t crc32;       /* covers every byte before this field */
};

/* We write the struct's bytes straight to disk, so its layout IS the file
 * format. If an edit changes the layout, the build stops here instead of
 * the files quietly becoming unreadable. */
_Static_assert(sizeof(struct dsm_record) == 48,
               "dsm_record must be exactly 48 bytes");
_Static_assert(offsetof(struct dsm_record, crc32) == 44,
               "crc32 must be the last 4 bytes of the record");
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "the on-disk format assumes a little-endian host");

void record_init(struct dsm_record *r, enum dsm_kind kind, uint16_t source_id);
void record_seal(struct dsm_record *r);
bool record_intact(const struct dsm_record *r);

#endif
