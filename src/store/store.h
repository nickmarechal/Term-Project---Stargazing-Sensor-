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
 * Durability today: store_append() hands the bytes to the kernel. It does
 * not wait for them to reach the SD card (no fsync), so a power cut can
 * still lose the most recent records. */

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

/* Opaque: only store.c can see inside. */
struct dsm_store;

enum store_status store_open(const char *path, struct dsm_store **out);
enum store_status store_append(struct dsm_store *s, const struct dsm_record *r);
enum store_status store_close(struct dsm_store *s);
struct store_recovery store_get_recovery(const struct dsm_store *s);

#endif
