#include "resource_hook_background.h"

static int bytes_equal(const char *a, const char *b, uint32_t size) {
    uint32_t i;
    for (i = 0; i < size; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

int rh_launcher_background_parse(const char *data, uint32_t size,
                                  struct rh_launcher_background *out) {
    static const char system_config[] = "version=1\nmode=system\n";
    static const char image_prefix[] = "version=1\nmode=image\nimage=";
    static const char themes[] = "themes/";
    static const char root[] = RH_APP_FILES_ROOT;
    const uint32_t prefix_length = sizeof(image_prefix) - 1u;
    const uint32_t root_length = sizeof(root) - 1u;
    const uint32_t themes_length = sizeof(themes) - 1u;
    char parsed[RH_PATH];
    const char *image;
    uint32_t length, i, theme_end, start;

    /* Bound the entire read before examining caller bytes, including sizes
     * near UINT32_MAX. No accepted document can exceed this limit. */
    if (!data || !out || root_length >= RH_PATH ||
        size > prefix_length + (RH_PATH - 1u - root_length) + 1u)
        return -1;
    for (i = 0; i < size; i++)
        if (!data[i]) return -1;

    if (size == sizeof(system_config) - 1u &&
        bytes_equal(data, system_config, sizeof(system_config) - 1u)) {
        out->image[0] = 0;
        return 0;
    }
    if (size <= prefix_length ||
        !bytes_equal(data, image_prefix, prefix_length) || data[size - 1u] != '\n')
        return -1;
    image = data + prefix_length;
    length = size - prefix_length - 1u;
    if (length >= RH_PATH - root_length || length <= themes_length ||
        !bytes_equal(image, themes, themes_length))
        return -1;

    theme_end = themes_length;
    while (theme_end < length && image[theme_end] != '/') {
        unsigned char c = (unsigned char)image[theme_end];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return -1;
        theme_end++;
    }
    if (theme_end == length || theme_end == themes_length ||
        theme_end - themes_length > 64u)
        return -1;

    start = theme_end + 1u;
    for (i = start; i <= length; i++) {
        if (i == length || image[i] == '/') {
            uint32_t component_length = i - start;
            if (!component_length ||
                (component_length == 1u && image[start] == '.') ||
                (component_length == 2u && image[start] == '.' &&
                 image[start + 1u] == '.'))
                return -1;
            start = i + 1u;
        } else {
            unsigned char c = (unsigned char)image[i];
            if (c < 32u || c == 127u || c == '\\' || c == ':') return -1;
        }
    }
    /* Stage bytes before committing so even overlapping input/output is safe.
     * Explicit bounded loops keep freestanding builds free of libc helpers. */
    for (i = 0; i < root_length; i++) parsed[i] = root[i];
    for (i = 0; i < length; i++) parsed[root_length + i] = image[i];
    parsed[root_length + length] = 0;
    for (i = 0; i <= root_length + length; i++) out->image[i] = parsed[i];
    return 0;
}
