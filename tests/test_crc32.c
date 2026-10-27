#include "check.h"
#include "suites.h"
#include "common/crc32.h"

#include <stdint.h>
#include <string.h>

void test_crc32(void)
{
    /* Published check values for the standard CRC-32. Matching two of them
     * means the polynomial, the bit order and the final XOR are all right. */
    uint32_t got = dsm_crc32("123456789", 9);
    CHECK(got == 0xCBF43926u, "check value: got %08x, want cbf43926", got);

    const char *fox = "The quick brown fox jumps over the lazy dog";
    got = dsm_crc32(fox, strlen(fox));
    CHECK(got == 0x414FA339u, "fox: got %08x, want 414fa339", got);

    got = dsm_crc32("", 0);
    CHECK(got == 0x00000000u, "empty input: got %08x, want 00000000", got);

    /* This is the property the storage layer relies on: a record that
     * picked up any single wrong bit must not still pass its CRC. */
    unsigned char msg[9];
    memcpy(msg, "123456789", sizeof msg);
    uint32_t clean = dsm_crc32(msg, sizeof msg);
    int missed = 0;
    for (size_t bit = 0; bit < 8 * sizeof msg; bit++) {
        msg[bit / 8] ^= (unsigned char)(1u << (bit % 8));
        if (dsm_crc32(msg, sizeof msg) == clean)
            missed++;
        msg[bit / 8] ^= (unsigned char)(1u << (bit % 8));
    }
    CHECK(missed == 0, "%d of %zu single-bit flips left the CRC unchanged",
          missed, 8 * sizeof msg);
}
