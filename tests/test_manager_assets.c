/* Host-only service adapter: exercise the production asset resolver against
 * installer-extracted package resources, with size/missing/directory faults,
 * without a firmware image. The generator's real .bin files are the fixtures,
 * so a passing run also proves the .inc byte lengths match the shipped images.
 */
#include "manager_assets.h"
#include "canopus_services.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum fault_kind { NO_FAULT, MISSING, SHORT, ISDIR, CACHED, INVALID };
static const char *data_root;

static int data_path(const struct canopus_context_v1 *ctx, char *out, uint32_t size) {
    (void)ctx;
    return snprintf(out, size, "%s", data_root);
}
static int file_stat(const struct canopus_context_v1 *ctx, const char *path,
                      struct canopus_fs_stat *out) {
    struct stat st;
    (void)ctx;
    if (stat(path, &st)) return -errno;
    out->size = (uint32_t)st.st_size;
    out->is_dir = S_ISDIR(st.st_mode);
    return 0;
}

/* Apply a fault to one resource by locating the path the resolver will use. */
static void fault_path(char *out, const char *name) {
    snprintf(out, 512, "%s/%s", data_root, name);
}

int main(int argc, char **argv) {
    char path[512];
    int i, rc, target = 0;
    const char *resolved;
    struct stat st;
    const struct canopus_fs_obj fs = {
        .get_data_path = data_path, .stat = file_stat
    };
    const struct canopus_context_v1 ctx = {
        .struct_size = sizeof(ctx), .fs = &fs
    };
    assert(argc == 3);
    data_root = argv[1];
    unsigned fault = (unsigned)atoi(argv[2]);
    assert(fault <= INVALID);

    /* ensure() before install() must report state, not resolve a path. */
    resolved = (const char *)1;
    assert(rh_manager_asset_ensure(RH_MANAGER_ASSET_LOGO, &resolved) == CANOPUS_SVC_ERR_STATE);
    assert(resolved == NULL);

    assert(rh_manager_assets_install(&ctx) == 0);
    assert(rh_manager_assets_install(&ctx) == 0);

    if (fault == INVALID) {
        resolved = (const char *)1;
        assert(rh_manager_asset_ensure(RH_MANAGER_ASSET_COUNT, &resolved) == CANOPUS_SVC_ERR_ARGUMENT);
        assert(resolved == NULL);
        assert(rh_manager_asset_path(RH_MANAGER_ASSET_COUNT) == NULL);
        puts("Manager assets: out-of-range id rejected");
        return 0;
    }

    /* Mutate one resolved resource according to the fault. */
    fault_path(path, fault == MISSING ? "status-running.bin" : "logo.bin");
    if (fault == MISSING) {
        assert(unlink(path) == 0);
        target = RH_MANAGER_ASSET_STATUS_RUNNING;
    } else if (fault == SHORT) {
        assert(stat(path, &st) == 0 && st.st_size > 0);
        FILE *f = fopen(path, "wb");
        assert(f);
        for (i = 0; i + 1 < (int)st.st_size; i++) fputc(0, f);
        fclose(f);
        target = RH_MANAGER_ASSET_LOGO;
    } else if (fault == ISDIR) {
        assert(unlink(path) == 0 && mkdir(path, 0700) == 0);
        target = RH_MANAGER_ASSET_LOGO;
    }

    for (i = 0; i < RH_MANAGER_ASSET_COUNT; i++) {
        resolved = (const char *)1;
        rc = rh_manager_asset_ensure((enum rh_manager_asset)i, &resolved);
        if ((int)i == target && fault != NO_FAULT && fault != CACHED) {
            assert(rc < 0 && resolved == NULL);
            assert(rh_manager_asset_path((enum rh_manager_asset)i) == NULL);
        } else {
            assert(rc == 0 && resolved && resolved[0] == '/');
            assert(rh_manager_asset_path((enum rh_manager_asset)i) == resolved);
        }
    }

    /* A second pass must return the latched paths; an unfaulted resolver is
     * stable, while a faulted resource is re-checked and fails the same way. */
    for (i = 0; i < RH_MANAGER_ASSET_COUNT; i++) {
        resolved = (const char *)1;
        rc = rh_manager_asset_ensure((enum rh_manager_asset)i, &resolved);
        if ((int)i == target && fault != NO_FAULT && fault != CACHED) {
            assert(rc < 0 && resolved == NULL);
        } else {
            assert(rc == 0 && resolved);
        }
    }

    puts("Manager assets: extracted resource resolution, size/missing/dir faults passed");
    return 0;
}
