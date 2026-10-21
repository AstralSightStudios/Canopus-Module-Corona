#include "resource_hook_platform.h"
#include "canopus_services.h"

static const struct canopus_context_v1 *fs_context;

int rh_platform_bind_fs(const struct canopus_context_v1 *ctx) {
    if (!canopus_services_has(ctx, CANOPUS_SVC_FS, 1) || !ctx->fs ||
        !ctx->fs->open || !ctx->fs->read || !ctx->fs->write ||
        !ctx->fs->close) return CANOPUS_SVC_ERR_UNSUPPORTED;
    if (fs_context && fs_context != ctx) return CANOPUS_SVC_ERR_STATE;
    fs_context = ctx;
    return 0;
}

int rh_platform_open(const char *path, int mode) {
    uint32_t flags;
    if (!fs_context) return CANOPUS_SVC_ERR_UNSUPPORTED;
    if (mode == RH_FILE_READ) flags = CANOPUS_FS_READ;
    else if (mode == RH_FILE_REPLACE) flags = CANOPUS_FS_WRITE;
    else return CANOPUS_SVC_ERR_ARGUMENT;
    return fs_context->fs->open(fs_context, path, flags);
}

int rh_platform_read(int handle, void *out, uint32_t size) {
    if (!fs_context) return CANOPUS_SVC_ERR_UNSUPPORTED;
    return fs_context->fs->read(fs_context, handle, out, size);
}

int rh_platform_write(int handle, const void *data, uint32_t size) {
    if (!fs_context) return CANOPUS_SVC_ERR_UNSUPPORTED;
    return fs_context->fs->write(fs_context, handle, data, size);
}

void rh_platform_close(int handle) {
    if (fs_context) (void)fs_context->fs->close(fs_context, handle);
}
