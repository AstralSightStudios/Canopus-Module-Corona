#ifndef RESOURCE_HOOK_H
#define RESOURCE_HOOK_H
#include <stdint.h>
#define RH_RULES 256
#define RH_PATH 256
/* Native internal://files/ roots differ between Band 10 Pro and Band 11. */
#if defined(RH_TARGET_1043) && RH_TARGET_1043
#define RH_APP_FILES_ROOT "/data/files/ng.lst.corona/"
#else
#define RH_APP_FILES_ROOT "/data/quickapp/files/ng.lst.corona/"
#endif
#define RH_CONFIG_PATH RH_APP_FILES_ROOT "mappings.tsv"
#define RH_RELOAD_SIGNAL_PATH RH_APP_FILES_ROOT "reload.request"
#define RH_RELOAD_RESULT_PATH RH_APP_FILES_ROOT "reload.result"
#define RH_THEME_ROOT RH_APP_FILES_ROOT "themes/"
#define RH_SYSTEM_DESTINATION "@system"
/* Directory rules map source prefixes into the app-private themes tree and
 * append the unmatched suffix; file rules map one exact source path. The
 * reserved RH_SYSTEM_DESTINATION exact-file rule explicitly keeps the firmware
 * resource and masks any broader directory rule. The longest match wins. */
struct rh_rule { char source[RH_PATH], destination[RH_PATH]; };
#define RH_INDEX_DIRECTORY 1u
#define RH_INDEX_SYSTEM 2u
struct rh_indexed_rule {
    uint16_t source_offset, destination_offset;
    uint8_t source_length, destination_length, flags, reserved;
};
/* Immutable after construction except references, which the owner changes only
 * under its lock. The string pool immediately follows count indexed records. */
struct rh_snapshot {
    uint32_t references, allocation_bytes, pool_bytes, count;
    struct rh_indexed_rule rules[];
};
struct rh_mapping_view {
    const struct rh_rule *rules;
    uint32_t count;
    const struct rh_snapshot *snapshot;
};
struct rh_allocator {
    void *cookie;
    void *(*alloc)(void *, uint32_t);
    void (*free)(void *, void *);
};
struct rh_mapping_view rh_rules_view(const struct rh_rule *, uint32_t);
struct rh_mapping_view rh_snapshot_view(const struct rh_snapshot *);
/* Snapshot contents are validated by their builder; legacy rules are fully
 * validated here. Only builder-produced immutable snapshots may be used. */
int rh_validate_view(const struct rh_mapping_view *);
typedef int (*rh_open_fn)(void *,const char *,int);
struct rh_state {
    struct rh_rule *rules; uint32_t count, redirected, fallback;
    rh_open_fn original; void *driver; int installed;
    uint32_t rules_capacity;
};
int rh_validate_rules(const struct rh_rule *, uint32_t);
/* Legacy state backing is caller-owned and must have sufficient capacity.
 * Input/staging storage must not overlap the live backing array. */
int rh_configure(struct rh_state *,const struct rh_rule *,uint32_t);
int rh_open(struct rh_state *,void *,const char *,int);
int rh_posix_open(struct rh_state *,void *,const char *,int);
int rh_posix_open_view(struct rh_state *, const struct rh_mapping_view *,
                       void *, const char *, int);
int rh_reinstall_posix(struct rh_state *, void *, rh_open_fn *, rh_open_fn, rh_open_fn);
int rh_install(struct rh_state *,void *,rh_open_fn *,rh_open_fn);
int rh_resolve(const struct rh_state *, const char *, char out[RH_PATH]);
int rh_resolve_view(const struct rh_mapping_view *, const char *, char out[RH_PATH]);
/* TSV destinations are themes/... relative to app files (or @system).
 * Parsed rules store target-specific absolute native destinations. */
int rh_parse_config(const char *, uint32_t, struct rh_rule *, uint32_t, uint32_t *);
/* Caller owns staging and text buffers; neither may overlap live state.
 * Reader returns bytes read, zero EOF, or a negative error. */
#define RH_CONFIG_BYTES 32768u
typedef int (*rh_read_fn)(void *, void *, uint32_t);
int rh_read_staged_config(rh_read_fn, void *, char *, uint32_t,
                          struct rh_rule *, uint32_t *);
int rh_read_config(struct rh_state *, rh_read_fn, void *, char *, uint32_t,
                   struct rh_rule *);
/* Builders leave *out unchanged on failure; empty success sets it to NULL.
 * parse_snapshot requires caller text to remain immutable through return.
 * -5: reader failure/malformed return, -6: input size overflow, -7: OOM.
 * read_snapshot uses a temporary RH_CONFIG_BYTES buffer, not a rule array. */
int rh_parse_snapshot(const char *, uint32_t, const struct rh_allocator *,
                       struct rh_snapshot **);
int rh_read_snapshot(rh_read_fn, void *, const struct rh_allocator *,
                      struct rh_snapshot **);
void rh_free_snapshot(const struct rh_allocator *, struct rh_snapshot *);
int rh_snapshot_equal(const struct rh_snapshot *, const struct rh_snapshot *);
#endif
