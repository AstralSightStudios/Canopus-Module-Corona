#ifndef RESOURCE_HOOK_H
#define RESOURCE_HOOK_H
#include <stdint.h>
#define RH_RULES 64
#define RH_PATH 256
/* Prefix rules: source directory `/resource/icon/` maps to destination
 * directory `/data/canopus/themes/current/icon/`; the unmatched suffix is
 * appended verbatim. Longest source prefix wins. */
struct rh_rule { char source[RH_PATH], destination[RH_PATH]; };
struct rh_mapping_view { const struct rh_rule *rules; uint32_t count; };
typedef int (*rh_open_fn)(void *,const char *,int);
struct rh_state {
    struct rh_rule rules[RH_RULES]; uint32_t count, redirected, fallback;
    rh_open_fn original; void *driver; int installed;
};
int rh_validate_rules(const struct rh_rule *, uint32_t);
int rh_configure(struct rh_state *,const struct rh_rule *,uint32_t);
int rh_open(struct rh_state *,void *,const char *,int);
int rh_posix_open(struct rh_state *,void *,const char *,int);
int rh_posix_open_view(struct rh_state *, const struct rh_mapping_view *,
                       void *, const char *, int);
int rh_reinstall_posix(struct rh_state *, void *, rh_open_fn *, rh_open_fn, rh_open_fn);
int rh_install(struct rh_state *,void *,rh_open_fn *,rh_open_fn);
int rh_resolve(const struct rh_state *, const char *, char out[RH_PATH]);
int rh_resolve_view(const struct rh_mapping_view *, const char *, char out[RH_PATH]);
int rh_parse_config(const char *, uint32_t, struct rh_rule *, uint32_t, uint32_t *);
/* Caller owns staging and text buffers; neither may overlap live state.
 * Reader returns bytes read, zero EOF, or a negative error. */
#define RH_CONFIG_BYTES 32768u
typedef int (*rh_read_fn)(void *, void *, uint32_t);
int rh_read_staged_config(rh_read_fn, void *, char *, uint32_t,
                          struct rh_rule *, uint32_t *);
int rh_read_config(struct rh_state *, rh_read_fn, void *, char *, uint32_t,
                   struct rh_rule *);
#endif
