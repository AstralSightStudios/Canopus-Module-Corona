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
    RH_MANAGER_ASSET_CHECK,
    RH_MANAGER_ASSET_UNCHECK,
    RH_MANAGER_ASSET_LOGO,
    RH_MANAGER_ASSET_COUNT
};

/* Bind the module-private data root populated by the installer's resource
 * extraction. No resource is read or decoded; calls and returned paths belong
 * to the serialized owner. */
int rh_manager_assets_install(const struct canopus_context_v1 *);
/* Validate an extracted package resource by exact size and return its absolute
 * path. The path remains valid for the module lifetime; the image bytes are
 * loaded lazily by the UI image node. */
int rh_manager_asset_ensure(enum rh_manager_asset, const char **out_path);
/* NULL until ensure succeeded. Does not perform I/O. */
const char *rh_manager_asset_path(enum rh_manager_asset);

#endif
