#ifndef RESOURCE_HOOK_P67_BATTERY_H
#define RESOURCE_HOOK_P67_BATTERY_H

#include <stddef.h>
#include <stdint.h>

/* LVGL .043 ARGB8888 stores B,G,R,A bytes on this little-endian target.
 * Key the completed, battery-only image, never the display framebuffer.
 * Preserve nonblack alpha (including antialiasing); do not key near-black.
 * Bounds are checked before any write; row padding is left untouched. */
static inline int p67_battery_key_black(void *pixels, size_t size,
                                       size_t stride, unsigned width,
                                       unsigned height) {
    unsigned x, y;
    unsigned char *p = pixels;
    if (!p || !width || !height || width > 128u || height > 128u ||
        stride < (size_t)width * 4u || stride > size / height)
        return 0;
    for (y = 0; y < height; ++y) {
        unsigned char *row = p + (size_t)y * stride;
        for (x = 0; x < width; ++x) {
            unsigned char *pixel = row + (size_t)x * 4u;
            if (!(pixel[0] | pixel[1] | pixel[2])) pixel[3] = 0;
        }
    }
    return 1;
}

#endif
