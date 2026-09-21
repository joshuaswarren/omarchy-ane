/* Offline check only: no hardware and no firmware file modification.
 * cc -Wall -Wextra -Werror tools/test_t6021_diag_marker.c -o /tmp/test-marker && /tmp/test-marker
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../ane/t6021/ane_t6021_diag_marker.h"

int main(void)
{
    unsigned char image[0x300];
    const unsigned char expected[] = {
        0x80,0x0c,0x80,0xd2,0x80,0xb0,0xb0,0xf2,
        0x40,0x00,0xc0,0xf2,0x21,0x86,0x8a,0x52,
        0x41,0xa6,0xa9,0x72,0x01,0x00,0x00,0xb9,
        0x9f,0x3f,0x03,0xd5,0x00,0x00,0x00,0x14
    };
    for (unsigned int bits = 0; bits < 16; ++bits) {
        bool diag = bits & 1, load = bits & 2;
        bool boot = bits & 4, transport = bits & 8;
        assert(ane_t6021_diag_options_ok(diag, load, boot, transport) ==
               (!diag || (load && !boot && !transport)));
    }
    memset(image, 0xa5, sizeof(image));
    ane_t6021_diag_patch(image);
    assert(!memcmp(image + 0x204, expected, sizeof(expected)));
    for (unsigned int i = 0; i < sizeof(image); ++i)
        if (i < 0x204 || i >= 0x204 + sizeof(expected))
            assert(image[i] == 0xa5);
    puts("PASS marker bytes, patch boundary, and all option combinations (offline only)");
    return 0;
}
