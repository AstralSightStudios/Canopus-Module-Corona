#include "resource_hook.h"
/* Format: source path TAB destination path LF. Directory paths end in '/'
 * and append their unmatched suffix; file paths map exactly. # starts a comment
 * only at the beginning of a line. No escape syntax, truncation, partial apply
 * or silently ignored malformed rule is allowed. Caller provides staging. */
int rh_parse_config(const char *text, uint32_t size, struct rh_rule *staging,
                    uint32_t capacity, uint32_t *count) {
    uint32_t p = 0, n = 0;
    if (!text || !staging || !count || capacity > RH_RULES) return -1;
    while (p < size) {
        uint32_t start = p, end, split, i, a, b;
        while (p < size && text[p] != '\n') {
            if (!text[p]) return -2;
            p++;
        }
        end = p;
        if (p < size) p++;
        if (end > start && text[end-1] == '\r') end--;
        if (end == start || text[start] == '#') continue;
        split = start;
        while (split < end && text[split] != '\t') split++;
        if (split == end || split == start || split+1 == end || n == capacity) return -3;
        a = split-start; b = end-split-1;
        if (a >= RH_PATH || b >= RH_PATH) return -4;
        for (i = 0; i < RH_PATH; i++) {
            staging[n].source[i] = i < a ? text[start+i] : 0;
            staging[n].destination[i] = i < b ? text[split+1+i] : 0;
        }
        n++;
    }
    *count = n;
    return 0;
}

int rh_read_staged_config(rh_read_fn read, void *cookie, char *text,
                          uint32_t capacity, struct rh_rule *staging,
                          uint32_t *count) {
    uint32_t used = 0;
    int got, rc;
    char extra;
    if (!read || !text || !staging || !count ||
        !capacity || capacity > RH_CONFIG_BYTES) return -1;
    while (used < capacity) {
        got = read(cookie, text+used, capacity-used);
        if (got < 0 || (uint32_t)got > capacity-used) return -5;
        if (!got) break;
        used += (uint32_t)got;
    }
    if (used == capacity && read(cookie, &extra, 1) != 0) return -6;
    rc = rh_parse_config(text, used, staging, RH_RULES, count);
    if (rc) return rc;
    return rh_validate_rules(staging, *count);
}

int rh_read_config(struct rh_state *s, rh_read_fn read, void *cookie,
                   char *text, uint32_t capacity, struct rh_rule *staging) {
    uint32_t count;
    int rc;
    if (!s || s->installed || !read || !text || !staging ||
        !capacity || capacity > RH_CONFIG_BYTES) return -1;
    rc = rh_read_staged_config(read, cookie, text, capacity, staging, &count);
    if (rc) return rc;
    return rh_configure(s, staging, count);
}
