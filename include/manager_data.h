#ifndef RH_MANAGER_DATA_H
#define RH_MANAGER_DATA_H
#include <stdint.h>
#include "canopus_services.h"

#define RH_MANAGER_DEFAULT "@default"
#define RH_MANAGER_SYSTEM "@system"
#define RH_MANAGER_BLUR "@blur"
#define RH_MANAGER_FEATURES 5u
#define RH_MANAGER_ERR_INVALID (-22)
#define RH_MANAGER_ERR_ACTIVE (-2001)
#define RH_MANAGER_ERR_LIMIT (-2002)

struct rh_manager_data;
struct rh_manager_metadata {
    char id[65], name[129], author[129], version[65], description[1025];
    char targets[2094]; /* all sixteen 128-byte declarations, separated by 、 */
    uint64_t version_code;
    uint8_t has_author, has_version, has_version_code, available;
};
struct rh_manager_mix_item {
    char key[256]; /* feature:<id> or exact resource path */
    char label[256];
    char choice[65];
    char choice_name[129];
    uint8_t feature;
};
struct rh_manager_option {
    char id[65], name[129], preview[256];
    uint8_t selected;
};
/* All callbacks run on the serialized module/UI owner. Optional adapters must
 * return actual errors; absence never fabricates successful cleanup/resolution. */
struct rh_manager_data_hooks {
    int (*remove_tree)(void *cookie, const char *absolute_path);
    int (*quickapp_icon)(void *cookie, const char *package, char out[256]);
    void *cookie;
};
/* Creates one shared catalog; no page owns a copy. Caller destroys on unload.
 * Files are relative to fs.get_data_path. Missing registry is an empty install,
 * corrupt registry/order/overrides is an error, not an empty list. */
int rh_manager_data_create(const struct canopus_context_v1 *,
                           const struct rh_manager_data_hooks *, struct rh_manager_data **);
void rh_manager_data_destroy(struct rh_manager_data *);
int rh_manager_data_refresh(struct rh_manager_data *);
const char *rh_manager_data_root(const struct rh_manager_data *);
uint32_t rh_manager_data_order_count(const struct rh_manager_data *);
int rh_manager_data_order_get(struct rh_manager_data *, uint32_t index,
                              char id[65], struct rh_manager_metadata *);
int rh_manager_data_metadata(struct rh_manager_data *, const char *id,
                             struct rh_manager_metadata *);
int rh_manager_data_move(struct rh_manager_data *, uint32_t from, uint32_t to);
/* Mix items use stable partition: explicit selections first, then defaults.
 * Features retain their fixed order and paths retain bytewise sorted order. */
int rh_manager_data_mix_count(struct rh_manager_data *, uint32_t *);
int rh_manager_data_mix_get(struct rh_manager_data *, uint32_t index,
                            struct rh_manager_mix_item *);
int rh_manager_data_option_count(struct rh_manager_data *, const char *key, uint32_t *);
int rh_manager_data_option_get(struct rh_manager_data *, const char *key,
                               uint32_t index, struct rh_manager_option *);
int rh_manager_data_choose(struct rh_manager_data *, const char *key, const char *choice);
/* Builds candidate mappings/backgrounds/calendar under canonical
 * active/<nonzero uint32 decimal> (no leading zeros or trailing slash).
 * Caller publishes the three files together using its internal reload
 * transaction and then calls complete_apply. Until then old AND new package
 * references remain protected. Never implies refresh has completed. */
int rh_manager_data_prepare_apply(struct rh_manager_data *, const char *config_dir);
/* Success requires active-config still names the exact pending generation.
 * Then retires old canonical config trios and old overlay bytes, retaining
 * empty directory shells. Invalid/changed pointers leave protection intact. */
int rh_manager_data_complete_apply(struct rh_manager_data *, int success);
int rh_manager_data_delete(struct rh_manager_data *, const char *id);
const char *rh_manager_data_error(const struct rh_manager_data *);
#endif
