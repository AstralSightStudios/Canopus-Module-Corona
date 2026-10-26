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
 * Files are relative to fs.get_data_path. Before the first completed legacy
 * import, refresh merges old quickapp packs without replacing registered IDs.
 * Missing registry is an empty install; corrupt registry/order/overrides is an
 * error, not an empty list. */
int rh_manager_data_create(const struct canopus_context_v1 *,
                           const struct rh_manager_data_hooks *, struct rh_manager_data **);
void rh_manager_data_destroy(struct rh_manager_data *);
int rh_manager_data_refresh(struct rh_manager_data *);
const char *rh_manager_data_root(const struct rh_manager_data *);
uint32_t rh_manager_data_order_count(const struct rh_manager_data *);
int rh_manager_data_order_get(struct rh_manager_data *, uint32_t index,
                              char id[65], struct rh_manager_metadata *);
/* Reads installed display metadata without compiling mapping/calendar rules.
 * corona.json wins; canora.json is used only when corona.json is absent. */
int rh_manager_data_metadata(struct rh_manager_data *, const char *id,
                             struct rh_manager_metadata *);
/* Theme id of the most recent install completed through install_step in this
   catalog lifetime, empty before the first one. Display only: kept as the
   catalog's own record, the UI has no notice surface to reveal it. */
int rh_manager_data_last_installed(const struct rh_manager_data *, char id[65]);
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
 * Then retires old canonical config trios and old overlay bytes. FS v2 also
 * removes drained directories; FS v1 retains empty shells. Invalid/changed
 * pointers leave protection intact. */
int rh_manager_data_complete_apply(struct rh_manager_data *, int success);
int rh_manager_data_delete(struct rh_manager_data *, const char *id);
/* Install a verified/extracted CRPack from staging/<owner-owned-name>. The ZIP
 * receiver supplies its disk-verified hash. Manifests are read one at a time
 * (64 KiB maximum; 256 mappings), never a ZIP directory or per-page inventory.
 * generation must be nonzero and unused for this theme. Success registers an
 * immutable themes/<themeId>/<generation> and does not apply it. A new commit
 * consumes staging by rename; a hash duplicate leaves staging for caller
 * cleanup. A durable pending record completes interrupted registration on
 * create/refresh/retry. Native errors, including
 * RH_MANAGER_ERR_ACTIVE / LIMIT, follow the other catalog APIs. */
int rh_manager_data_install(struct rh_manager_data *, const char *staging_relative,
                            uint32_t generation, const uint8_t hash[32], char installed_id[65]);
/* Cooperative installation: begin returns 0 accepted, negative native error;
 * step returns 0 working, 1 complete (id filled), negative native error.
 * Each step consumes at most one directory entry OR performs one file sync.
 * Manifest/schema validation retains its 64 KiB / 256-mapping bound and runs
 * in one step. Only the serialized owner may access staging: freeze all bytes
 * before begin and keep them immutable until completion/abort and journal
 * recovery. A pending journal certifies completed validation/sync; recovery
 * never rescans the tree. Mutation/refresh APIs return BUSY during this job.
 * Abort closes handles/releases RAM; committed intent is preserved on disk,
 * so callers MUST successfully refresh before cleaning abandoned staging.
 * The synchronous compatibility API above loops over begin/step. */
int rh_manager_data_install_begin(struct rh_manager_data *, const char *staging_relative,
                                  uint32_t generation, const uint8_t hash[32]);
int rh_manager_data_install_step(struct rh_manager_data *, char installed_id[65]);
/* Cancel before the registry publication point. Returns 0 revoked, 1 already
 * committed, or a native error. Unlike abort, this removes pending intent. */
int rh_manager_data_install_cancel(struct rh_manager_data *, const uint8_t hash[32]);
void rh_manager_data_install_abort(struct rh_manager_data *);
uint32_t rh_manager_data_revision(struct rh_manager_data *);
/* 1 found, 0 absent, negative protocol code (BAD=1, BUSY=2, IO=5). */
int rh_manager_data_find_hash(struct rh_manager_data *, const uint8_t hash[32], char id[65]);
/* size is input capacity/output payload bytes, excluding the 12-byte frame.
 * A 468-byte capacity suffices. Returns negative protocol codes BAD=1,
 * BUSY=2, IO=5, LIMIT=8, CHANGED=10. Pages are themeId ASCII order; expected
 * revision 0 starts a new enumeration, otherwise it must equal the registry. */
int rh_manager_data_connect_list(struct rh_manager_data *, uint32_t expected_revision,
                                 uint32_t index, uint8_t *payload, uint32_t *size);
const char *rh_manager_data_error(const struct rh_manager_data *);
#endif
