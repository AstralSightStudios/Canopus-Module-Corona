"""Exercise the production FS adapter through a fake Supervisor service object.

The service owns handle identity and error codes. These host checks do not
execute firmware I/O or the SDK's stock-modlib registration transport.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get('CANOPUS_ROOT', ROOT.parent / 'Canopus-Private')).resolve()


class ServiceIOContract(unittest.TestCase):
    def test_binding_handles_modes_and_errors(self):
        fixture = r'''
#include "resource_hook_platform.h"
#include "canopus_services.h"
#include <assert.h>
#include <string.h>

static const struct canopus_context_v1 *expected_context;
static int open_result, read_result, write_result, close_result;
static unsigned opens, reads, writes, closes;
static uint32_t expected_flags;
static int32_t service_open(const struct canopus_context_v1 *ctx,
                             const char *path, uint32_t flags) {
    assert(ctx == expected_context && !strcmp(path, "/data/fixture"));
    assert(flags == expected_flags);
    opens++;
    return open_result;
}
static int32_t service_read(const struct canopus_context_v1 *ctx, int32_t handle,
                             void *out, uint32_t size) {
    assert(ctx == expected_context && handle == 0 && size == 4);
    reads++;
    if (read_result > 0) memcpy(out, "test", (unsigned)read_result);
    return read_result;
}
static int32_t service_write(const struct canopus_context_v1 *ctx, int32_t handle,
                              const void *data, uint32_t size) {
    assert(ctx == expected_context && handle == 0 && size == 4);
    assert(!memcmp(data, "test", size));
    writes++;
    return write_result;
}
static int32_t service_close(const struct canopus_context_v1 *ctx, int32_t handle) {
    assert(ctx == expected_context && handle == 0);
    closes++;
    return close_result;
}

int main(void) {
    char buffer[4] = {0};
    struct canopus_fs_obj fs = {
        .open = service_open, .read = service_read,
        .write = service_write, .close = service_close,
    };
    struct canopus_services_v1 services = {
        .struct_size = sizeof(services),
        .group_version = {[CANOPUS_SVC_FS] = 1},
    };
    struct canopus_context_v1 ctx = {
        .struct_size = sizeof(ctx), .services = &services, .fs = &fs,
    }, foreign;
    expected_context = &ctx;

    assert(rh_platform_open("/data/fixture", RH_FILE_READ) == CANOPUS_SVC_ERR_UNSUPPORTED);
    assert(rh_platform_read(0, buffer, sizeof(buffer)) == CANOPUS_SVC_ERR_UNSUPPORTED);
    assert(rh_platform_write(0, "test", 4) == CANOPUS_SVC_ERR_UNSUPPORTED);
    rh_platform_close(0);
    assert(!opens && !reads && !writes && !closes);
    assert(rh_platform_bind_fs(0) == CANOPUS_SVC_ERR_UNSUPPORTED);
    services.group_version[CANOPUS_SVC_FS] = 0;
    assert(rh_platform_bind_fs(&ctx) == CANOPUS_SVC_ERR_UNSUPPORTED);
    services.group_version[CANOPUS_SVC_FS] = 1;
    ctx.fs = 0;
    assert(rh_platform_bind_fs(&ctx) == CANOPUS_SVC_ERR_UNSUPPORTED);
    ctx.fs = &fs;
    fs.open = 0;
    assert(rh_platform_bind_fs(&ctx) == CANOPUS_SVC_ERR_UNSUPPORTED);
    fs.open = service_open; fs.read = 0;
    assert(rh_platform_bind_fs(&ctx) == CANOPUS_SVC_ERR_UNSUPPORTED);
    fs.read = service_read; fs.write = 0;
    assert(rh_platform_bind_fs(&ctx) == CANOPUS_SVC_ERR_UNSUPPORTED);
    fs.write = service_write; fs.close = 0;
    assert(rh_platform_bind_fs(&ctx) == CANOPUS_SVC_ERR_UNSUPPORTED);
    fs.close = service_close;
    assert(!opens && !reads && !writes && !closes);

    assert(rh_platform_bind_fs(&ctx) == 0);
    assert(rh_platform_bind_fs(&ctx) == 0);
    foreign = ctx;
    assert(rh_platform_bind_fs(&foreign) == CANOPUS_SVC_ERR_STATE);
    /* Unsupported mode must not open a file or accidentally truncate it. */
    assert(rh_platform_open("/data/fixture", 0x26) == CANOPUS_SVC_ERR_ARGUMENT);
    assert(rh_platform_open("/data/fixture", 0) == CANOPUS_SVC_ERR_ARGUMENT);
    assert(!opens);

    expected_flags = CANOPUS_FS_READ;
    open_result = 0; /* Zero is a valid module-private handle. */
    assert(rh_platform_open("/data/fixture", RH_FILE_READ) == 0);
    read_result = 3;
    assert(rh_platform_read(0, buffer, sizeof(buffer)) == 3);
    assert(!memcmp(buffer, "tes", 3));
    read_result = 0;
    assert(rh_platform_read(0, buffer, sizeof(buffer)) == 0);
    read_result = CANOPUS_SVC_ERR_IO;
    assert(rh_platform_read(0, buffer, sizeof(buffer)) == CANOPUS_SVC_ERR_IO);
    close_result = CANOPUS_SVC_ERR_IO;
    rh_platform_close(0);
    assert(closes == 1);
    open_result = CANOPUS_SVC_ERR_NOTFOUND;
    assert(rh_platform_open("/data/fixture", RH_FILE_READ) == -RH_ENOENT);
    open_result = CANOPUS_SVC_ERR_ACCESS;
    assert(rh_platform_open("/data/fixture", RH_FILE_READ) == CANOPUS_SVC_ERR_ACCESS);

    expected_flags = CANOPUS_FS_WRITE;
    open_result = 0;
    assert(rh_platform_open("/data/fixture", RH_FILE_REPLACE) == 0);
    write_result = 2;
    assert(rh_platform_write(0, "test", 4) == 2);
    write_result = CANOPUS_SVC_ERR_NOSPACE;
    assert(rh_platform_write(0, "test", 4) == CANOPUS_SVC_ERR_NOSPACE);
    close_result = 0;
    rh_platform_close(0);
    assert(opens == 4 && reads == 3 && writes == 2 && closes == 2);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='rh-service-io-') as tmp:
            path = Path(tmp)
            (path / 'test.c').write_text(fixture)
            binary = path / 'test'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11',
                            '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined',
                            '-I' + str(ROOT / 'include'), '-I' + str(SDK / 'sdk/c'),
                            str(ROOT / 'src/platform_fs.c'), str(path / 'test.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
