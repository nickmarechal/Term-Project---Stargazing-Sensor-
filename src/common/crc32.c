#include "common/crc32.h"

/* Bit-at-a-time rather than the faster table version. We checksum about
 * three 48-byte records a second, so speed doesn't matter, and this is the
 * version that can be worked through by hand. */
uint32_t dsm_crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1u)
                crc = (crc >> 1) ^ 0xEDB88320u;
            else
                crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFFu;
}
