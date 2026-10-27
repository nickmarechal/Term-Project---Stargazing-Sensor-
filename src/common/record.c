#include "common/record.h"
#include "common/crc32.h"

#include <string.h>

/* Zero the whole record first. Any byte left uninitialized would end up in
 * the CRC, and valgrind on the Pi reports that as a use of uninitialized
 * memory even when the result looks fine. */
void record_init(struct dsm_record *r, enum dsm_kind kind, uint16_t source_id)
{
    memset(r, 0, sizeof *r);
    r->schema_ver = DSM_SCHEMA_VER;
    r->kind = (uint16_t)kind;
    r->source_id = source_id;
}

void record_seal(struct dsm_record *r)
{
    r->crc32 = dsm_crc32(r, offsetof(struct dsm_record, crc32));
}

bool record_intact(const struct dsm_record *r)
{
    return r->crc32 == dsm_crc32(r, offsetof(struct dsm_record, crc32));
}
