#ifndef DSM_CRC32_H
#define DSM_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* The standard CRC-32 (the one zlib, Ethernet and PNG use), so the output
 * can be checked against published values. */
uint32_t dsm_crc32(const void *data, size_t len);

#endif
