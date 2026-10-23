#ifndef RH_MANAGER_ASSETS_H
#define RH_MANAGER_ASSETS_H

struct canopus_context_v1;

enum rh_manager_asset {
    RH_MANAGER_ASSET_STATUS_RUNNING,
    RH_MANAGER_ASSET_STATUS_CONFIG_ERROR,
    RH_MANAGER_ASSET_STATUS_UNRESPONSIVE,
    RH_MANAGER_ASSET_RELOAD,
    RH_MANAGER_ASSET_RELOAD_WIDE,
    RH_MANAGER_ASSET_DELETE,
    RH_MANAGER_ASSET_DELETE_WIDE,
    RH_MANAGER_ASSET_NONE,
    RH_MANAGER_ASSET_CONNECT,
    RH_MANAGER_ASSET_CHOOSE,
    RH_MANAGER_ASSET_DOWNLOAD,
    RH_MANAGER_ASSET_ERROR,
    RH_MANAGER_ASSET_SUCCESS,
    RH_MANAGER_ASSET_LOGO,
    RH_MANAGER_ASSET_COUNT
};

/* Bind the module-private data root and create assets/. No image is decoded
 * until requested. Calls and returned paths belong to the serialized owner. */
int rh_manager_assets_install(const struct canopus_context_v1 *);
/* Writes an immutable, versioned LVGL v9 image on first use. On success the
 * returned absolute path remains valid for the module lifetime. */
int rh_manager_asset_ensure(enum rh_manager_asset, const char **out_path);
/* NULL until ensure succeeded. Does not perform I/O. */
const char *rh_manager_asset_path(enum rh_manager_asset);

#endif
