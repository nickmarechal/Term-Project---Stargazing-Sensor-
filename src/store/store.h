#ifndef DSM_STORE_H
#define DSM_STORE_H

#include "common/record.h"

#include <stddef.h>
#include <stdint.h>

/* A store file is one header followed by records, back to back:
 *
 *   [ header: 24 bytes ][ record 0: 48 ][ record 1: 48 ] ...
 *
 * Records are only ever added at the end. Nothing is rewritten in place.
 *
 * Crash recovery: store_open() checks every record of an existing file and
 * cuts the file off at the first one that is incomplete or damaged, which is
 * what a crash in the middle of a write leaves behind. store_get_recovery()
 * says what it kept and what it cut.
 *
 * Durability: store_append() hands the bytes to the kernel and does not
 * wait for them to reach the SD card. store_sync() waits (fsync). A power
 * cut loses at most what was appended since the last store_sync(), so the
 * caller decides the bound by how often it syncs. The library has no clock
 * on purpose: stored picks the interval, and these tests need no timing. */

#define DSM_STORE_MAGIC "DSKY"

/* The first 24 bytes of every store file. */
struct dsm_store_header {
    char     magic[4];         /* "DSKY", no terminating zero */
    uint16_t schema_ver;       /* DSM_SCHEMA_VER of the records inside */
    uint16_t record_size;      /* sizeof(struct dsm_record) */
    uint64_t created_real_ns;  /* CLOCK_REALTIME when the file was made */
    uint32_t reserved;         /* always 0; puts crc32 last with no padding */
    uint32_t crc32;            /* covers every byte before this field */
};

_Static_assert(sizeof(struct dsm_store_header) == 24,
               "dsm_store_header must be exactly 24 bytes");
_Static_assert(offsetof(struct dsm_store_header, crc32) == 20,
               "crc32 must be the last 4 bytes of the header");

enum store_status {
    STORE_OK = 0,
    STORE_ERR_IO,      /* a system call failed; errno says why */
    STORE_ERR_FORMAT,  /* the file exists but is not a store file; untouched */
    STORE_ERR_BADREC,  /* the record's CRC does not match; nothing written */
    STORE_ERR_BROKEN,  /* an earlier write failed; no more appends */
};

/* What recovery did when the file was opened. Both are 0 for a new file. */
struct store_recovery {
    uint64_t kept;       /* intact records found */
    uint64_t cut_bytes;  /* bytes cut off the end; 0 if the file was clean */
};

/* What this handle has done since store_open(). Recovery's own work is in
 * store_recovery, not here. */
struct store_counters {
    uint64_t records;  /* records appended */
    uint64_t writes;   /* times records were handed to the kernel */
    uint64_t fsyncs;   /* successful store_sync() calls */
};

/* Opaque: only store.c can see inside. */
struct dsm_store;

enum store_status store_open(const char *path, struct dsm_store **out);
enum store_status store_append(struct dsm_store *s, const struct dsm_record *r);

/* fsync: returns once everything appended so far is on the card. If fsync
 * fails we no longer know what reached the card, so the handle is marked
 * broken (STORE_ERR_IO now, STORE_ERR_BROKEN after) and the caller should
 * exit and let recovery run on the next open. */
enum store_status store_sync(struct dsm_store *s);

/* Syncs, then closes and frees, even if the sync fails (STORE_ERR_IO).
 * A broken handle is not synced: it is closed and freed, and the result
 * is STORE_ERR_BROKEN. */
enum store_status store_close(struct dsm_store *s);
struct store_recovery store_get_recovery(const struct dsm_store *s);
struct store_counters store_get_counters(const struct dsm_store *s);

typedef void (*store_visit_fn)(const struct dsm_record *r, void *ctx);

/* Read-only: checks the header, then calls fn (if not NULL) on each intact
 * record in order, stopping at the first short or damaged one, exactly
 * where recovery would cut. out says what recovery would keep and cut.
 * It never changes the file, so it is safe while stored is writing: it
 * reads the file as it was when the scan began, and a record stored is
 * halfway through writing just shows up as a few tail bytes. */
enum store_status store_scan(const char *path, store_visit_fn fn, void *ctx,
                             struct store_recovery *out);

#endif
