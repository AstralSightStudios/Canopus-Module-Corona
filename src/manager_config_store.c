#include "manager_config_store.h"
#include "canopus_services.h"
#include "canopus_memory.h"

#define CONFIG_ROOT "/data/canopus/m.corona/"
#define CONFIG_POINTER CONFIG_ROOT "active-config"
#define CONFIG_TEMP CONFIG_ROOT "active-config.part"
#define POINTER_BYTES 32u

static const struct canopus_context_v1 *config_context;
static char config_directory[CANOPUS_FS_PATH_MAX];
static unsigned config_busy;
static int config_bound_error;

/* One canonical spelling gives each immutable generation a single identity. */
static int normalize_relative(const char *relative, char canonical[POINTER_BYTES]) {
    static const char prefix[] = "active/";
    uint32_t length, i, value = 0, digits;
    if (!relative) return CANOPUS_SVC_ERR_ARGUMENT;
    length = (uint32_t)canopus_strnlen(relative, POINTER_BYTES);
    if (length == POINTER_BYTES || length < sizeof(prefix)) return CANOPUS_SVC_ERR_ARGUMENT;
    for (i = 0; i < sizeof(prefix) - 1u; ++i)
        if (relative[i] != prefix[i]) return CANOPUS_SVC_ERR_ARGUMENT;
    if (relative[length - 1u] == '/') --length;
    digits = length - (uint32_t)(sizeof(prefix) - 1u);
    if (!digits || digits > 10u || relative[sizeof(prefix) - 1u] == '0')
        return CANOPUS_SVC_ERR_ARGUMENT;
    for (i = (uint32_t)(sizeof(prefix) - 1u); i < length; ++i) {
        uint32_t digit;
        if (relative[i] < '0' || relative[i] > '9') return CANOPUS_SVC_ERR_ARGUMENT;
        digit = (uint32_t)(relative[i] - '0');
        if (value > (UINT32_MAX - digit) / 10u) return CANOPUS_SVC_ERR_ARGUMENT;
        value = value * 10u + digit;
    }
    if (!value) return CANOPUS_SVC_ERR_ARGUMENT;
    canopus_memcpy(canonical, relative, length);
    canonical[length] = 0;
    return 0;
}

static void absolute_directory(const char *relative, char out[CANOPUS_FS_PATH_MAX]) {
    uint32_t root = sizeof(CONFIG_ROOT) - 1u;
    uint32_t length = (uint32_t)canopus_strlen(relative);
    /* Validated relative names have at most 17 bytes. */
    canopus_memcpy(out, CONFIG_ROOT, root);
    canopus_memcpy(out + root, relative, length);
    out[root + length] = '/';
    out[root + length + 1u] = 0;
}

static int read_pointer(const struct canopus_context_v1 *ctx,
                        char relative[POINTER_BYTES]) {
    char record[POINTER_BYTES + 1u], canonical[POINTER_BYTES];
    uint32_t used = 0;
    int fd, count, rc = 0, close_rc;
    fd = ctx->fs->open(ctx, CONFIG_POINTER, CANOPUS_FS_READ);
    if (fd == CANOPUS_SVC_ERR_NOTFOUND) {
        relative[0] = 0;
        return 0;
    }
    if (fd < 0) return fd;
    while (used < POINTER_BYTES) {
        count = ctx->fs->read(ctx, fd, record + used, POINTER_BYTES - used);
        if (count < 0) { rc = count; break; }
        if ((uint32_t)count > POINTER_BYTES - used) { rc = CANOPUS_SVC_ERR_IO; break; }
        if (!count) break;
        used += (uint32_t)count;
    }
    close_rc = ctx->fs->close(ctx, fd);
    if (!rc && close_rc < 0) rc = close_rc;
    if (rc) return rc;
    /* Reaching capacity is already larger than any valid pointer. Reject it
     * without treating a truncated prefix as a complete record. */
    if (!used || used == POINTER_BYTES || record[used - 1u] != '\n')
        return CANOPUS_SVC_ERR_ARGUMENT;
    record[used - 1u] = 0;
    if (canopus_strnlen(record, used) != used - 1u)
        return CANOPUS_SVC_ERR_ARGUMENT;
    rc = normalize_relative(record, canonical);
    if (rc) return rc;
    if (canopus_strcmp(record, canonical)) return CANOPUS_SVC_ERR_ARGUMENT;
    canopus_memcpy(relative, canonical, used);
    return 0;
}

int rh_manager_config_bind(const struct canopus_context_v1 *ctx) {
    char root[CANOPUS_FS_PATH_MAX], relative[POINTER_BYTES];
    uint32_t length;
    int rc;
    if (!ctx || ctx->struct_size < sizeof(*ctx) || !ctx->fs ||
        !ctx->fs->get_data_path || !ctx->fs->open || !ctx->fs->read ||
        !ctx->fs->write || !ctx->fs->close || !ctx->fs->rename)
        return CANOPUS_SVC_ERR_UNSUPPORTED;
    if (config_busy) return CANOPUS_SVC_ERR_BUSY;
    if (config_context)
        return config_context == ctx ? config_bound_error : CANOPUS_SVC_ERR_STATE;
    config_busy = 1;
    canopus_memset(root, 0xff, sizeof(root));
    rc = ctx->fs->get_data_path(ctx, root, sizeof(root));
    if (rc < 0) goto done;
    length = (uint32_t)canopus_strnlen(root, sizeof(root));
    if (length == sizeof(CONFIG_ROOT) - 2u &&
        root[length - 1u] != '/') {
        root[length++] = '/';
        root[length] = 0;
    }
    if (length != sizeof(CONFIG_ROOT) - 1u || canopus_strcmp(root, CONFIG_ROOT)) {
        rc = CANOPUS_SVC_ERR_ARGUMENT;
        goto done;
    }
    /* Root validation authorizes candidate preparation and a replacement
     * publication even when the existing active pointer cannot be used. Cache
     * that error until commit repairs it; bind itself never becomes a poll. */
    config_context = ctx;
    rc = read_pointer(ctx, relative);
    config_bound_error = rc;
    if (rc) goto done;
    if (relative[0]) absolute_directory(relative, config_directory);
    else canopus_memcpy(config_directory, CONFIG_ROOT, sizeof(CONFIG_ROOT));
done:
    config_busy = 0;
    return rc;
}

const char *rh_manager_config_current_dir(void) {
    return config_context && !config_bound_error && config_directory[0]
        ? config_directory : 0;
}

int rh_manager_config_candidate(const char *relative_dir, char out[256]) {
    char canonical[POINTER_BYTES];
    int rc;
    if (!out) return CANOPUS_SVC_ERR_ARGUMENT;
    if (!config_context) return CANOPUS_SVC_ERR_STATE;
    rc = normalize_relative(relative_dir, canonical);
    if (rc) return rc;
    absolute_directory(canonical, out);
    return 0;
}

int rh_manager_config_commit(const char *relative_dir) {
    char record[POINTER_BYTES], directory[CANOPUS_FS_PATH_MAX];
    uint32_t length, used = 0;
    int fd, rc, close_rc;
    if (!config_context) return CANOPUS_SVC_ERR_STATE;
    if (config_busy) return CANOPUS_SVC_ERR_BUSY;
    rc = normalize_relative(relative_dir, record);
    if (rc) return rc;
    absolute_directory(record, directory);
    length = (uint32_t)canopus_strlen(record);
    record[length++] = '\n';
    config_busy = 1;
    fd = config_context->fs->open(config_context, CONFIG_TEMP, CANOPUS_FS_WRITE);
    if (fd < 0) { rc = fd; goto done; }
    while (used < length) {
        rc = config_context->fs->write(config_context, fd, record + used, length - used);
        if (rc < 0) break;
        if (!rc || (uint32_t)rc > length - used) { rc = CANOPUS_SVC_ERR_IO; break; }
        used += (uint32_t)rc;
    }
    close_rc = config_context->fs->close(config_context, fd);
    if (rc >= 0) rc = close_rc < 0 ? close_rc : 0;
    if (!rc) {
        rc = config_context->fs->rename(config_context, CONFIG_TEMP, CONFIG_POINTER);
        if (rc >= 0) rc = 0;
    }
    if (rc < 0) {
        /* A stale .part never becomes the active generation at startup. */
        if (config_context->fs->remove)
            (void)config_context->fs->remove(config_context, CONFIG_TEMP);
        goto done;
    }
    canopus_memcpy(config_directory, directory, canopus_strlen(directory) + 1u);
    config_bound_error = 0;
done:
    config_busy = 0;
    return rc;
}
