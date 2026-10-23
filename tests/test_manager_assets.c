/* Host-only service adapter: exercise the production asset installer and
 * decoder with short writes and injected failures, without a firmware image. */
#include "manager_assets.h"
#include "canopus_services.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

enum fault_kind {
    NO_FAULT, ZERO_WRITE, FAILED_WRITE, FAILED_CLOSE, FAILED_RENAME, FAILED_ALLOC,
    EXISTING_FILES
};
static const char *data_root;
static FILE *files[32];
static unsigned fault, allocations, writes;

static int data_path(const struct canopus_context_v1 *ctx, char *out, uint32_t size) {
    (void)ctx;
    return snprintf(out, size, "%s", data_root);
}
static int make_directory(const struct canopus_context_v1 *ctx, const char *path) {
    (void)ctx;
    return mkdir(path, 0700) ? -errno : 0;
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
static int open_file(const struct canopus_context_v1 *ctx, const char *path,
                      uint32_t flags) {
    unsigned i;
    (void)ctx;
    assert(flags == CANOPUS_FS_WRITE);
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (files[i]) continue;
        files[i] = fopen(path, "wb");
        return files[i] ? (int)i : -errno;
    }
    return -24;
}
static int write_file(const struct canopus_context_v1 *ctx, int fd,
                       const void *data, uint32_t size) {
    (void)ctx;
    writes++;
    if (fault == ZERO_WRITE) return 0;
    if (fault == FAILED_WRITE) return CANOPUS_SVC_ERR_IO;
    /* Force multiple writes for every decoder output block. */
    if (size > 13u) size = 13u;
    return (int)fwrite(data, 1, size, files[fd]);
}
static int close_file(const struct canopus_context_v1 *ctx, int fd) {
    int rc;
    (void)ctx;
    rc = fclose(files[fd]);
    files[fd] = NULL;
    return fault == FAILED_CLOSE ? CANOPUS_SVC_ERR_IO : rc;
}
static int rename_file(const struct canopus_context_v1 *ctx,
                        const char *from, const char *to) {
    (void)ctx;
    return fault == FAILED_RENAME ? CANOPUS_SVC_ERR_IO : rename(from, to);
}
static int remove_file(const struct canopus_context_v1 *ctx, const char *path) {
    (void)ctx;
    return unlink(path);
}
static void *allocate(const struct canopus_context_v1 *ctx, uint32_t size) {
    void *out;
    (void)ctx;
    assert(size == 4096u);
    if (fault == FAILED_ALLOC) return NULL;
    assert(allocations == 0u);
    out = malloc(size);
    assert(out);
    allocations++;
    return out;
}
static void release(const struct canopus_context_v1 *ctx, void *pointer) {
    (void)ctx;
    assert(allocations == 1u);
    allocations--;
    free(pointer);
}

int main(int argc, char **argv) {
    int i, rc;
    unsigned before;
    const char *path = (const char *)1;
    const struct canopus_fs_obj fs = {
        .get_data_path = data_path, .mkdir = make_directory, .stat = file_stat,
        .open = open_file, .write = write_file, .close = close_file,
        .rename = rename_file, .remove = remove_file
    };
    const struct canopus_mem_obj mem = { .alloc = allocate, .free = release };
    const struct canopus_context_v1 ctx = {
        .struct_size = sizeof(ctx), .fs = &fs, .mem = &mem
    };
    assert(argc == 3);
    data_root = argv[1];
    fault = (unsigned)atoi(argv[2]);
    assert(fault <= EXISTING_FILES);
    assert(rh_manager_asset_ensure(RH_MANAGER_ASSET_LOGO, &path) == CANOPUS_SVC_ERR_STATE);
    assert(path == NULL);
    assert(rh_manager_assets_install(&ctx) == 0);
    assert(rh_manager_assets_install(&ctx) == 0);
    if (fault != NO_FAULT && fault != EXISTING_FILES) {
        rc = rh_manager_asset_ensure(RH_MANAGER_ASSET_STATUS_RUNNING, &path);
        assert(rc < 0 && path == NULL && allocations == 0u);
        assert(rh_manager_asset_path(RH_MANAGER_ASSET_STATUS_RUNNING) == NULL);
        fault = NO_FAULT;
    }
    for (i = 0; i < RH_MANAGER_ASSET_COUNT; i++) {
        assert(rh_manager_asset_path(i) == NULL);
        assert(rh_manager_asset_ensure(i, &path) == 0 && path);
        assert(rh_manager_asset_path(i) == path);
        before = writes;
        assert(rh_manager_asset_ensure(i, &path) == 0 && writes == before);
    }
    assert(allocations == 0u);
    for (i = 0; i < 32; i++) assert(files[i] == NULL);
    if (fault == EXISTING_FILES) assert(writes == 0u);
    assert(rh_manager_asset_ensure(RH_MANAGER_ASSET_COUNT, &path) == CANOPUS_SVC_ERR_ARGUMENT);
    assert(path == NULL);
    puts("Manager assets: short writes, retry, cache and allocation checks passed");
    return 0;
}
