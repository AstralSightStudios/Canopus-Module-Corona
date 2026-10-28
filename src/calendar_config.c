#include "resource_hook_calendar.h"

static int equal(const char *a, uint32_t n, const char *b) {
    uint32_t i;
    for (i = 0; i < n; ++i) if (!b[i] || a[i] != b[i]) return 0;
    return !b[n];
}
static int number(const char *s, uint32_t n, int32_t *out, int32_t lo, int32_t hi) {
    uint32_t i = 0, value = 0, negative = 0;
    if (!n) return -1;
    if (s[0] == '-') { negative = 1; i = 1; }
    if (i == n) return -1;
    for (; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9' || value > 100000u) return -1;
        value = value * 10u + (unsigned)(s[i] - '0');
    }
    *out = negative ? -(int32_t)value : (int32_t)value;
    return *out < lo || *out > hi ? -1 : 0;
}
void rh_calendar_defaults(struct rh_calendar_config *c) {
    unsigned i, j;
    c->pool_used = 0;
    c->custom = 0;
    for (i = 0; i < 3; ++i) {
        struct rh_calendar_item *p = &c->items[i];
        p->type = i ? RH_CALENDAR_TEXT : RH_CALENDAR_NONE;
        p->align = 2;
        p->x = 0;
#if defined(RH_TARGET_P65) && RH_TARGET_P65
        p->y = i == 1 ? 28 : 12;
#else
        p->y = i == 1 ? 34 : 17;
#endif
        p->size = p->color = p->count = 0;
        p->color_native = 1;
        for (j = 0; j < 31; ++j) p->context[j] = 0;
    }
}
static int image_path(const char *s, uint32_t n) {
    uint32_t i, start = 0;
    if (n < 8 || n >= RH_PATH - (sizeof(RH_APP_FILES_ROOT) - 1u) ||
        !equal(s, 7, "themes/") || !equal(s + n - 4, 4, ".bin")) return 0;
    for (i = 0; i <= n; ++i) {
        if (i == n || s[i] == '/') {
            uint32_t len = i - start;
            if (!len || equal(s + start, len, ".") || equal(s + start, len, "..")) return 0;
            start = i + 1;
        } else if ((unsigned char)s[i] < 32u || s[i] == ':' || s[i] == '\\' || s[i] == 127) return 0;
    }
    return 1;
}
/* Validate UTF-8 without accepting overlong encodings, surrogates or controls. */
static int text_ok(const char *s, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        uint32_t ch = (unsigned char)s[i++], extra = 0, min = 0;
        if (ch < 128) { if (ch < 32 || ch == 127) return 0; continue; }
        if (ch >= 0xc2 && ch <= 0xdf) { extra = 1; min = 128; ch &= 31; }
        else if (ch >= 0xe0 && ch <= 0xef) { extra = 2; min = 2048; ch &= 15; }
        else if (ch >= 0xf0 && ch <= 0xf4) { extra = 3; min = 65536; ch &= 7; }
        else return 0;
        if (extra > n - i) return 0;
        while (extra--) {
            unsigned next = (unsigned char)s[i++];
            if ((next & 0xc0) != 0x80) return 0;
            ch = (ch << 6) | (next & 63);
        }
        if (ch < min || ch > 0x10ffff || (ch >= 0xd800 && ch <= 0xdfff) ||
            (ch >= 0x80 && ch <= 0x9f)) return 0;
    }
    return 1;
}
int rh_calendar_parse(const char *data, uint32_t size, struct rh_calendar_config *c) {
    uint32_t pos = 10, seen = 0;
    if (!data || !c || size < 10 || size > RH_CALENDAR_CONFIG_BYTES ||
        !equal(data, 10, "version=1\n") || data[size - 1] != '\n') return -1;
    rh_calendar_defaults(c);
    while (pos < size) {
        const char *field[8]; uint32_t length[8], f, start = pos, id, j;
        int32_t value;
        struct rh_calendar_item *p;
        for (f = 0; f < 8; ++f) {
            field[f] = data + start;
            while (pos < size && data[pos] != '\t' && data[pos] != '\n') ++pos;
            length[f] = pos - start;
            if (pos == size || data[pos] != (f == 7 ? '\n' : '\t')) return -1;
            start = ++pos;
        }
        if (equal(field[0], length[0], "month")) id = 0;
        else if (equal(field[0], length[0], "day")) id = 1;
        else if (equal(field[0], length[0], "weekday")) id = 2;
        else return -1;
        if (seen & (1u << id)) return -1;
        seen |= 1u << id; p = &c->items[id];
        if (equal(field[1], length[1], "none")) p->type = RH_CALENDAR_NONE;
        else if (equal(field[1], length[1], "text")) p->type = RH_CALENDAR_TEXT;
        else if (equal(field[1], length[1], "image")) p->type = RH_CALENDAR_IMAGE;
        else return -1;
        if (equal(field[2], length[2], "top_left")) p->align = 1;
        else if (equal(field[2], length[2], "top_mid")) p->align = 2;
        else if (equal(field[2], length[2], "top_right")) p->align = 3;
        else if (equal(field[2], length[2], "center")) p->align = 9;
        else return -1;
        if (number(field[3], length[3], &p->x, -112, 112) ||
            number(field[4], length[4], &p->y, -112, 112) ||
            number(field[5], length[5], &value, 0, 72)) return -1;
        if (value && value < 8) return -1;
        p->size = (uint32_t)value;
        if (equal(field[6], length[6], "native")) p->color_native = 1;
        else {
            uint32_t color = 0;
            if (length[6] != 6) return -1;
            for (j = 0; j < 6; ++j) {
                unsigned ch = (unsigned char)field[6][j], digit;
                if (ch >= '0' && ch <= '9') digit = ch - '0';
                else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
                else return -1;
                color = color * 16 + digit;
            }
            p->color_native = 0; p->color = color;
        }
        if (number(field[7], length[7], &value, 0, (int32_t)rh_calendar_context_count(id))) return -1;
        p->count = (uint32_t)value;
        if ((p->count && p->count != rh_calendar_context_count(id)) ||
            (p->type == RH_CALENDAR_IMAGE && (!p->count || p->size || !p->color_native)) ||
            (p->type == RH_CALENDAR_NONE && p->count)) return -1;
        for (j = 0; j < p->count; ++j) {
            uint32_t n, offset = c->pool_used;
            start = pos;
            while (pos < size && data[pos] != '\n') ++pos;
            if (pos == size) return -1;
            n = pos - start;
            if ((p->type == RH_CALENDAR_TEXT && (n > 63 || !text_ok(data + start, n))) ||
                (p->type == RH_CALENDAR_IMAGE && !image_path(data + start, n)) ||
                n + 1 > RH_CALENDAR_POOL_BYTES - offset) return -1;
            p->context[j] = (uint16_t)offset;
            for (f = 0; f < n; ++f) c->pool[offset + f] = data[start + f];
            c->pool[offset + n] = 0;
            c->pool_used += n + 1; ++pos;
        }
    }
    c->custom = (seen != 0);
    return 0;
}
const char *rh_calendar_context(const struct rh_calendar_config *c, unsigned item, unsigned index) {
    if (!c || item >= 3 || index >= c->items[item].count ||
        c->items[item].context[index] >= c->pool_used) return 0;
    return c->pool + c->items[item].context[index];
}
