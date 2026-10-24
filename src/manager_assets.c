#include "manager_assets.h"
#include "canopus_services.h"

#include <stdint.h>

struct asset_record {
    const char *name;
    uint32_t size;
};
#include "generated/manager_assets.inc"

static const struct canopus_context_v1 *asset_context;
static char asset_root[CANOPUS_FS_PATH_MAX];
static char asset_paths[RH_MANAGER_ASSET_COUNT][CANOPUS_FS_PATH_MAX];
static uint32_t verified_assets;

static uint32_t text_length(const char *text, uint32_t capacity) {
    uint32_t length = 0;
    while (length < capacity && text[length]) length++;
    return length;
}

static int join_path(char *out, const char *directory, const char *name) {
    uint32_t i, root = text_length(directory, CANOPUS_FS_PATH_MAX);
    uint32_t length = text_length(name, CANOPUS_FS_PATH_MAX);
    if (root + length + 2u > CANOPUS_FS_PATH_MAX)
        return CANOPUS_SVC_ERR_ARGUMENT;
    for (i = 0; i < root; i++) out[i] = directory[i];
    out[root++] = '/';
    for (i = 0; i < length; i++) out[root + i] = name[i];
    out[root + length] = 0;
    return 0;
}

/* Package resources are extracted by the installer into the module data root
 * as one flat file per asset. Nothing is decoded or written here: bind the
 * data root and resolve each declared resource by name. */
int rh_manager_assets_install(const struct canopus_context_v1 *ctx) {
    uint32_t length, i;
    int rc;
    if (!ctx || ctx->struct_size < sizeof(*ctx) || !ctx->fs ||
        !ctx->fs->get_data_path || !ctx->fs->stat)
        return CANOPUS_SVC_ERR_UNSUPPORTED;
    rc = ctx->fs->get_data_path(ctx, asset_root, sizeof(asset_root));
    if (rc < 0) return rc;
    length = text_length(asset_root, sizeof(asset_root));
    if (!length || length == sizeof(asset_root) || asset_root[0] != '/')
        return CANOPUS_SVC_ERR_ARGUMENT;
    while (length > 1u && asset_root[length - 1u] == '/') asset_root[--length] = 0;
    for (i = 0; i < RH_MANAGER_ASSET_COUNT; i++) {
        rc = join_path(asset_paths[i], asset_root, asset_records[i].name);
        if (rc) return rc;
    }
    verified_assets = 0;
    asset_context = ctx;
    return 0;
}

/* Verify an installed package resource by exact byte length and hand back its
 * absolute path. The image itself is loaded lazily by the UI image node. */
int rh_manager_asset_ensure(enum rh_manager_asset id, const char **out_path) {
    const struct asset_record *asset;
    struct canopus_fs_stat stat;
    int rc;
    if (out_path) *out_path = 0;
    if ((unsigned)id >= RH_MANAGER_ASSET_COUNT || !out_path)
        return CANOPUS_SVC_ERR_ARGUMENT;
    if (!asset_context) return CANOPUS_SVC_ERR_STATE;
    asset = &asset_records[id];
    if (verified_assets & (1u << id)) {
        *out_path = asset_paths[id];
        return 0;
    }
    rc = asset_context->fs->stat(asset_context, asset_paths[id], &stat);
    if (rc < 0) return rc;
    if (stat.is_dir) return CANOPUS_SVC_ERR_ISDIR;
    if (stat.size != asset->size) return CANOPUS_SVC_ERR_IO;
    verified_assets |= 1u << id;
    *out_path = asset_paths[id];
    return 0;
}

const char *rh_manager_asset_path(enum rh_manager_asset id) {
    return (unsigned)id < RH_MANAGER_ASSET_COUNT && (verified_assets & (1u << id))
        ? asset_paths[id] : 0;
}
