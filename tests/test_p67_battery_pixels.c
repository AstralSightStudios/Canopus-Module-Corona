#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "resource_hook_p67_battery.h"

int main(void) {
    uint8_t image[2 * 24], before[sizeof(image)];
    /* Two padded rows: black cutout, white backing, red charge, antialias. */
    const uint8_t row[24] = {
        0,0,0,255, 255,255,255,255, 0,0,255,255, 1,1,1,127,
        0,0,0,17, 0xa5,0xa5,0xa5,0xa5
    };
    memcpy(image, row, sizeof(row));
    memcpy(image + sizeof(row), row, sizeof(row));
    memcpy(before, image, sizeof(image));
    assert(p67_battery_key_black(image, sizeof(image), 24, 5, 2));
    for (unsigned y = 0; y < 2; ++y) {
        assert(image[y * 24 + 3] == 0);
        assert(image[y * 24 + 19] == 0);
        assert(!memcmp(image + y * 24 + 4, before + y * 24 + 4, 12));
        assert(!memcmp(image + y * 24 + 20, before + y * 24 + 20, 4));
    }
    /* Reapplying the key is idempotent. */
    memcpy(before, image, sizeof(image));
    assert(p67_battery_key_black(image, sizeof(image), 24, 5, 2));
    assert(!memcmp(before, image, sizeof(image)));
    assert(!p67_battery_key_black(image, 47, 24, 5, 2));
    assert(!p67_battery_key_black(image, sizeof(image), 19, 5, 2));
    assert(!p67_battery_key_black(image, sizeof(image), SIZE_MAX, 5, 2));
    assert(!p67_battery_key_black(image, sizeof(image), 24, 129, 2));
    assert(!p67_battery_key_black(image, sizeof(image), 24, 5, 129));
    assert(!p67_battery_key_black(image, sizeof(image), 24, 0, 2));
    assert(!p67_battery_key_black(image, sizeof(image), 24, 5, 0));
    assert(!p67_battery_key_black(NULL, sizeof(image), 24, 5, 2));
    assert(!memcmp(before, image, sizeof(image)));
    puts("p67 battery black-key pixels: ok");
    return 0;
}
