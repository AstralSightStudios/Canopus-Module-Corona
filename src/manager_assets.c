#include "manager_assets.h"
#include "canopus_services.h"

#include <stdint.h>

struct asset_record {
    const char *name;
    const unsigned char *packed;
    uint32_t packed_size, image_size;
};
#include "generated/manager_assets.inc"

static const struct canopus_context_v1 *asset_context;
static char asset_directory[CANOPUS_FS_PATH_MAX];
static char asset_paths[RH_MANAGER_ASSET_COUNT][CANOPUS_FS_PATH_MAX];
static uint32_t installed_assets;

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
    for (i = 0; i <= length; i++) out[root + i] = name[i];
    return 0;
}

int rh_manager_assets_install(const struct canopus_context_v1 *ctx) {
    char root[CANOPUS_FS_PATH_MAX];
    struct canopus_fs_stat stat;
    uint32_t length, i;
    int rc;
    if (!ctx || ctx->struct_size < sizeof(*ctx) || !ctx->fs || !ctx->mem ||
        !ctx->fs->get_data_path || !ctx->fs->mkdir || !ctx->fs->stat ||
        !ctx->fs->open || !ctx->fs->write || !ctx->fs->close ||
        !ctx->fs->rename || !ctx->fs->remove ||
        !ctx->mem->alloc || !ctx->mem->free)
        return CANOPUS_SVC_ERR_UNSUPPORTED;
    if (asset_context) return asset_context == ctx ? 0 : CANOPUS_SVC_ERR_STATE;
    for (i = 0; i < sizeof(root); i++) root[i] = 0;
    rc = ctx->fs->get_data_path(ctx, root, sizeof(root));
    if (rc < 0) return rc;
    length = text_length(root, sizeof(root));
    if (!length || length == sizeof(root) || root[0] != '/')
        return CANOPUS_SVC_ERR_ARGUMENT;
    while (length > 1u && root[length - 1u] == '/') root[--length] = 0;
    rc = join_path(asset_directory, root, "assets");
    if (rc) return rc;
    /* mkdir does not create missing parents; the SDK creates the module root
     * only on a file write. Explicitly ensure it before the assets directory. */
    rc = ctx->fs->mkdir(ctx, root);
    if (rc < 0 && rc != CANOPUS_SVC_ERR_EXIST) return rc;
    rc = ctx->fs->mkdir(ctx, asset_directory);
    if (rc < 0 && rc != CANOPUS_SVC_ERR_EXIST) return rc;
    rc = ctx->fs->stat(ctx, asset_directory, &stat);
    if (rc < 0) return rc;
    if (!stat.is_dir) return CANOPUS_SVC_ERR_NOTDIR;
    for (i = 0; i < RH_MANAGER_ASSET_COUNT; i++) {
        rc = join_path(asset_paths[i], asset_directory, asset_records[i].name);
        if (rc) return rc;
    }
    installed_assets = 0;
    asset_context = ctx;
    return 0;
}

static int write_full(int fd, const unsigned char *data, uint32_t size) {
    uint32_t written = 0;
    int rc;
    while (written < size) {
        rc = asset_context->fs->write(asset_context, fd, data + written, size - written);
        if (rc < 0) return rc;
        if (!rc || (uint32_t)rc > size - written) return CANOPUS_SVC_ERR_IO;
        written += (uint32_t)rc;
    }
    return 0;
}

/* Each three compressed bytes are stored as four binary base64 sextets
 * tagged with 0x80. All stored data words stay above current firmware/RAM
 * address ranges. Decode three bytes at a time without another buffer. */
struct packed_reader {
    const struct asset_record *asset;
    uint32_t position, word;
};
static unsigned char packed_byte(struct packed_reader *reader) {
    uint32_t i, at;
    unsigned char result;
    if (!(reader->position % 3u)) {
        at = (reader->position / 3u) * 4u;
        reader->word = 0;
        for (i = 0; i < 4u; i++)
            reader->word |= (uint32_t)(reader->asset->packed[at + i] & 63u) << (i * 6u);
    }
    result = (unsigned char)reader->word;
    reader->word >>= 8;
    reader->position++;
    return result;
}

/* Decode into a fixed circular history plus one small output block. */
static int unpack_asset(int fd, const struct asset_record *asset,
                         unsigned char history[4096]) {
    unsigned char block[256];
    struct packed_reader input = {asset, 0, 0};
    uint32_t output = 0, used = 0, length, distance, i;
    unsigned char tag, byte;
    int rc;
    while (output < asset->image_size) {
        if (input.position >= asset->packed_size) return CANOPUS_SVC_ERR_IO;
        tag = packed_byte(&input);
        if (tag < 128u) {
            length = (uint32_t)tag + 1u;
            distance = 0;
            if (length > asset->packed_size - input.position) return CANOPUS_SVC_ERR_IO;
        } else {
            length = (tag & 127u) + 3u;
            if (asset->packed_size - input.position < 2u) return CANOPUS_SVC_ERR_IO;
            distance = packed_byte(&input);
            distance |= (uint32_t)packed_byte(&input) << 8;
            distance++;
            if (distance > 4096u || distance > output) return CANOPUS_SVC_ERR_IO;
        }
        if (length > asset->image_size - output) return CANOPUS_SVC_ERR_IO;
        for (i = 0; i < length; i++) {
            byte = distance ? history[(output - distance) & 4095u] : packed_byte(&input);
            history[output & 4095u] = byte;
            block[used++] = byte;
            output++;
            if (used == sizeof(block)) {
                rc = write_full(fd, block, used);
                if (rc) return rc;
                used = 0;
            }
        }
    }
    if (input.position != asset->packed_size) return CANOPUS_SVC_ERR_IO;
    return used ? write_full(fd, block, used) : 0;
}

int rh_manager_asset_ensure(enum rh_manager_asset id, const char **out_path) {
    const struct asset_record *asset;
    struct canopus_fs_stat stat;
    unsigned char *history;
    char temporary[CANOPUS_FS_PATH_MAX];
    uint32_t length, i;
    int fd, rc, close_rc;
    if (out_path) *out_path = 0;
    if ((unsigned)id >= RH_MANAGER_ASSET_COUNT || !out_path)
        return CANOPUS_SVC_ERR_ARGUMENT;
    if (!asset_context) return CANOPUS_SVC_ERR_STATE;
    asset = &asset_records[id];
    if (installed_assets & (1u << id)) {
        *out_path = asset_paths[id];
        return 0;
    }
    rc = asset_context->fs->stat(asset_context, asset_paths[id], &stat);
    if (!rc && !stat.is_dir && stat.size == asset->image_size) goto installed;
    if (rc < 0 && rc != CANOPUS_SVC_ERR_NOTFOUND) return rc;
    if (!rc && stat.is_dir) return CANOPUS_SVC_ERR_ISDIR;
    length = text_length(asset_paths[id], sizeof(temporary));
    if (length + sizeof(".part") > sizeof(temporary)) return CANOPUS_SVC_ERR_ARGUMENT;
    for (i = 0; i < length; i++) temporary[i] = asset_paths[id][i];
    for (i = 0; i < sizeof(".part"); i++) temporary[length + i] = ".part"[i];
    history = asset_context->mem->alloc(asset_context, 4096u);
    if (!history) return CANOPUS_SVC_ERR_NOMEM;
    fd = asset_context->fs->open(asset_context, temporary, CANOPUS_FS_WRITE);
    if (fd < 0) {
        asset_context->mem->free(asset_context, history);
        return fd;
    }
    rc = unpack_asset(fd, asset, history);
    close_rc = asset_context->fs->close(asset_context, fd);
    asset_context->mem->free(asset_context, history);
    if (!rc && close_rc < 0) rc = close_rc;
    if (!rc) rc = asset_context->fs->rename(asset_context, temporary, asset_paths[id]);
    if (rc < 0) {
        (void)asset_context->fs->remove(asset_context, temporary);
        return rc;
    }
installed:
    installed_assets |= 1u << id;
    *out_path = asset_paths[id];
    return 0;
}

const char *rh_manager_asset_path(enum rh_manager_asset id) {
    return (unsigned)id < RH_MANAGER_ASSET_COUNT && (installed_assets & (1u << id))
        ? asset_paths[id] : 0;
}
