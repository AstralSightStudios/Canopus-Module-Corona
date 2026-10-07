#include "resource_hook_background.h"

static int bytes_equal(const char *a, const char *b, uint32_t size) {
    return rh_bytes_equal(a, b, size);
}

static void set_special(char *out, const char *special, uint32_t size) {
    uint32_t i;
    for (i = 0; i < size; i++) out[i] = special[i];
}

static int validate_and_prefix_image(const char *image, uint32_t length, char *out) {
    static const char themes[] = "themes/";
    static const char root[] = RH_APP_FILES_ROOT;
    const uint32_t root_length = sizeof(root) - 1u;
    const uint32_t themes_length = sizeof(themes) - 1u;
    uint32_t i, theme_end, start;

    if (root_length >= RH_PATH || length >= RH_PATH - root_length ||
        length <= themes_length || !bytes_equal(image, themes, themes_length))
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

    for (i = 0; i < root_length; i++) out[i] = root[i];
    for (i = 0; i < length; i++) out[root_length + i] = image[i];
    out[root_length + length] = 0;
    return 0;
}

static void copy_staged(struct rh_backgrounds *out, const struct rh_backgrounds *staged) {
    uint32_t i;
    for (i = 0; i < RH_PATH; i++) {
        out->launcher[i] = staged->launcher[i];
        if (!staged->launcher[i]) break;
    }
    for (i = 0; i < RH_PATH; i++) {
        out->control_center[i] = staged->control_center[i];
        if (!staged->control_center[i]) break;
    }
    for (i = 0; i < RH_PATH; i++) {
        out->notifications[i] = staged->notifications[i];
        if (!staged->notifications[i]) break;
    }
    for (i = 0; i < RH_PATH; i++) {
        out->widgets[i] = staged->widgets[i];
        if (!staged->widgets[i]) break;
    }
}

int rh_backgrounds_parse(const char *data, uint32_t size,
                         struct rh_backgrounds *out) {
    static const char v1_header[] = "version=1\n";
    static const char legacy_system[] = "version=1\nmode=system\n";
    static const char legacy_prefix[] = "version=1\nmode=image\nimage=";
    static const char k_launcher[] = "launcher=";
    static const char k_control[] = "control_center=";
    static const char k_notify[] = "notifications=";
    static const char k_widgets[] = "widgets=";
    struct rh_backgrounds staged;
    uint32_t i, pos;

    if (!data || !out || size < sizeof(v1_header) - 1u || size > 4u * RH_PATH + 128u)
        return -1;
    for (i = 0; i < size; i++)
        if (!data[i]) return -1;
    if (data[size - 1u] != '\n') return -1;

    /* Initialize staged output with empty (system) values. */
    staged.launcher[0] = 0;
    staged.control_center[0] = 0;
    staged.notifications[0] = 0;
    staged.widgets[0] = 0;

    /* Legacy single-launcher formats. */
    if (size == sizeof(legacy_system) - 1u &&
        bytes_equal(data, legacy_system, sizeof(legacy_system) - 1u)) {
        copy_staged(out, &staged);
        return 0;
    }
    if (size > sizeof(legacy_prefix) - 1u &&
        bytes_equal(data, legacy_prefix, sizeof(legacy_prefix) - 1u)) {
        const char *img = data + sizeof(legacy_prefix) - 1u;
        uint32_t img_len = size - (sizeof(legacy_prefix) - 1u) - 1u;
        if (validate_and_prefix_image(img, img_len, staged.launcher)) return -1;
        copy_staged(out, &staged);
        return 0;
    }

    /* Multi-background format: must start with version=1\n and have at least one key. */
    if (!bytes_equal(data, v1_header, sizeof(v1_header) - 1u)) return -1;
    pos = sizeof(v1_header) - 1u;
    uint32_t keys_seen = 0;

    while (pos < size) {
        uint32_t line_start = pos, line_end;
        while (pos < size && data[pos] != '\n') pos++;
        line_end = pos;
        if (pos < size && data[pos] == '\n') pos++;
        if (line_end == line_start) continue;

        if (line_end - line_start >= sizeof(k_launcher) - 1u &&
            bytes_equal(data + line_start, k_launcher, sizeof(k_launcher) - 1u)) {
            const char *val = data + line_start + sizeof(k_launcher) - 1u;
            uint32_t val_len = line_end - (line_start + sizeof(k_launcher) - 1u);
            if (!val_len || (val_len == 6u && bytes_equal(val, "system", 6u))) {
                staged.launcher[0] = 0;
            } else if (val_len == 4u && bytes_equal(val, "blur", 4u)) {
                set_special(staged.launcher, RH_BG_BLUR, sizeof(RH_BG_BLUR));
            } else if (validate_and_prefix_image(val, val_len, staged.launcher)) {
                return -1;
            }
            keys_seen++;
        } else if (line_end - line_start >= sizeof(k_control) - 1u &&
                   bytes_equal(data + line_start, k_control, sizeof(k_control) - 1u)) {
            const char *val = data + line_start + sizeof(k_control) - 1u;
            uint32_t val_len = line_end - (line_start + sizeof(k_control) - 1u);
            if (!val_len || (val_len == 6u && bytes_equal(val, "system", 6u))) {
                staged.control_center[0] = 0;
            } else if (val_len == 4u && bytes_equal(val, "blur", 4u)) {
                set_special(staged.control_center, RH_BG_BLUR, sizeof(RH_BG_BLUR));
            } else if (validate_and_prefix_image(val, val_len, staged.control_center)) {
                return -1;
            }
            keys_seen++;
        } else if (line_end - line_start >= sizeof(k_notify) - 1u &&
                   bytes_equal(data + line_start, k_notify, sizeof(k_notify) - 1u)) {
            const char *val = data + line_start + sizeof(k_notify) - 1u;
            uint32_t val_len = line_end - (line_start + sizeof(k_notify) - 1u);
            if (!val_len || (val_len == 6u && bytes_equal(val, "system", 6u))) {
                staged.notifications[0] = 0;
            } else if (val_len == 4u && bytes_equal(val, "blur", 4u)) {
                set_special(staged.notifications, RH_BG_BLUR, sizeof(RH_BG_BLUR));
            } else if (validate_and_prefix_image(val, val_len, staged.notifications)) {
                return -1;
            }
            keys_seen++;
        } else if (line_end - line_start >= sizeof(k_widgets) - 1u &&
                   bytes_equal(data + line_start, k_widgets, sizeof(k_widgets) - 1u)) {
            const char *val = data + line_start + sizeof(k_widgets) - 1u;
            uint32_t val_len = line_end - (line_start + sizeof(k_widgets) - 1u);
            if (!val_len || (val_len == 6u && bytes_equal(val, "system", 6u))) {
                staged.widgets[0] = 0;
            } else if (val_len == 4u && bytes_equal(val, "blur", 4u)) {
                set_special(staged.widgets, RH_BG_BLUR, sizeof(RH_BG_BLUR));
            } else if (validate_and_prefix_image(val, val_len, staged.widgets)) {
                return -1;
            }
            keys_seen++;
        } else {
            return -1;
        }
    }
    if (!keys_seen) return -1;

    copy_staged(out, &staged);
    return 0;
}

int rh_launcher_background_parse(const char *data, uint32_t size,
                                 struct rh_launcher_background *out) {
    struct rh_backgrounds bg;
    uint32_t i;
    if (!out || rh_backgrounds_parse(data, size, &bg)) return -1;
    for (i = 0; i < RH_PATH; i++) {
        out->image[i] = bg.launcher[i];
        if (!bg.launcher[i]) break;
    }
    return 0;
}
