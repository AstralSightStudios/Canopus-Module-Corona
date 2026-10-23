#include "manager_config_store.h"
#include "canopus_services.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROOT "/data/canopus/m.corona/"
#define POINTER ROOT "active-config"
#define TEMP ROOT "active-config.part"

static const char *data_root = ROOT;
static unsigned char pointer_data[128], temporary_data[128];
static uint32_t pointer_size, temporary_size, cursor;
static unsigned pointer_exists, pointer_opens, temporary_opens, reads, writes, closes, renames, removes;
static int root_error, root_unterminated, open_error, read_error, read_oversize;
static int write_error, write_zero, write_oversize, close_error, rename_error;
static int recursive_commit;
static struct canopus_context_v1 context;

static int32_t get_data_path(const struct canopus_context_v1 *ctx, char *out, uint32_t cap) {
    size_t length = strlen(data_root);
    assert(ctx == &context);
    if (root_error) return root_error;
    if (root_unterminated) { memset(out, 'x', cap); return 0; }
    assert(length < cap);
    memcpy(out, data_root, length + 1u);
    return (int32_t)length;
}
static int32_t file_open(const struct canopus_context_v1 *ctx, const char *path, uint32_t flags) {
    assert(ctx == &context);
    if (!strcmp(path, POINTER)) {
        assert(flags == CANOPUS_FS_READ);
        ++pointer_opens;
        if (open_error) return open_error;
        if (!pointer_exists) return CANOPUS_SVC_ERR_NOTFOUND;
        cursor = 0;
        return 0; /* Service descriptor zero is valid. */
    }
    assert(!strcmp(path, TEMP) && flags == CANOPUS_FS_WRITE);
    ++temporary_opens;
    if (open_error) return open_error;
    temporary_size = 0;
    return 1;
}
static int32_t file_read(const struct canopus_context_v1 *ctx, int32_t fd, void *out, uint32_t size) {
    assert(ctx == &context && fd == 0);
    ++reads;
    if (read_error && cursor >= 3u) return read_error;
    if (read_oversize) return (int32_t)size + 1;
    if (size > 3u) size = 3u;
    if (size > pointer_size - cursor) size = pointer_size - cursor;
    memcpy(out, pointer_data + cursor, size);
    cursor += size;
    return (int32_t)size;
}
static int32_t file_write(const struct canopus_context_v1 *ctx, int32_t fd, const void *data, uint32_t size) {
    assert(ctx == &context && fd == 1);
    ++writes;
    if (recursive_commit) {
        recursive_commit = 0;
        assert(rh_manager_config_commit("active/999") == CANOPUS_SVC_ERR_BUSY);
    }
    if (write_error && temporary_size >= 3u) return write_error;
    if (write_zero) return 0;
    if (write_oversize) return (int32_t)size + 1;
    if (size > 3u) size = 3u;
    assert(temporary_size + size <= sizeof(temporary_data));
    memcpy(temporary_data + temporary_size, data, size);
    temporary_size += size;
    return (int32_t)size;
}
static int32_t file_close(const struct canopus_context_v1 *ctx, int32_t fd) {
    assert(ctx == &context && (fd == 0 || fd == 1));
    ++closes;
    return close_error;
}
static int32_t file_rename(const struct canopus_context_v1 *ctx, const char *src, const char *dst) {
    assert(ctx == &context && !strcmp(src, TEMP) && !strcmp(dst, POINTER));
    ++renames;
    if (rename_error) return rename_error;
    memcpy(pointer_data, temporary_data, temporary_size);
    pointer_size = temporary_size;
    pointer_exists = 1;
    return 0;
}
static int32_t file_remove(const struct canopus_context_v1 *ctx, const char *path) {
    assert(ctx == &context && !strcmp(path, TEMP));
    ++removes;
    temporary_size = 0;
    return CANOPUS_SVC_ERR_IO; /* Cleanup failure cannot replace the first error. */
}
static const struct canopus_fs_obj fs = {
    .get_data_path = get_data_path, .open = file_open, .read = file_read,
    .write = file_write, .close = file_close, .rename = file_rename, .remove = file_remove
};
static struct canopus_context_v1 context = {
    .struct_size = sizeof(context), .fs = &fs
};
static void set_pointer(const char *data) {
    pointer_size = (uint32_t)strlen(data);
    assert(pointer_size <= sizeof(pointer_data));
    memcpy(pointer_data, data, pointer_size);
    pointer_exists = 1;
}
static void assert_pointer(const char *expected) {
    assert(pointer_exists && pointer_size == strlen(expected));
    assert(!memcmp(pointer_data, expected, pointer_size));
}
static void invalid_candidates(void) {
    static const char *invalid[] = {
        "", "active/", "active/0", "active/00", "active/01", "active/+1", "active/-1",
        "active/4294967296", "active/99999999999", "active/1/2", "active//1",
        "active/1//", "active/1/../2", "active/./1", "active/1\n", "active/ 1",
        "active/1 ", "/active/1", "../active/1", "active\\1", "Active/1", "active/1\t",
        "active/18446744073709551616", "active/000000000000000000000000000000000001"
    };
    char out[256];
    size_t i;
    unsigned opens = temporary_opens, moved = renames;
    memset(out, 'Q', sizeof(out));
    assert(rh_manager_config_candidate(NULL, out) == CANOPUS_SVC_ERR_ARGUMENT);
    assert(rh_manager_config_candidate("active/1", NULL) == CANOPUS_SVC_ERR_ARGUMENT);
    assert(rh_manager_config_commit(NULL) == CANOPUS_SVC_ERR_ARGUMENT);
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(rh_manager_config_candidate(invalid[i], out) == CANOPUS_SVC_ERR_ARGUMENT);
        assert(out[0] == 'Q' && out[sizeof(out) - 1u] == 'Q');
        assert(rh_manager_config_commit(invalid[i]) == CANOPUS_SVC_ERR_ARGUMENT);
    }
    assert(temporary_opens == opens && renames == moved);
    assert(!rh_manager_config_candidate("active/1/", out));
    assert(!strcmp(out, ROOT "active/1/"));
    assert(!rh_manager_config_candidate("active/4294967295", out));
    assert(!strcmp(out, ROOT "active/4294967295/"));
}
static void commit_failures(void) {
    unsigned moved, closed, removed;
    assert(!rh_manager_config_commit("active/42/"));
    assert_pointer("active/42\n");
    assert(!strcmp(rh_manager_config_current_dir(), ROOT "active/42/"));
    assert(writes >= 4u); /* Three-byte writes exercise complete-write handling. */
    moved = renames;
    closed = closes;
    open_error = CANOPUS_SVC_ERR_NOSPACE;
    assert(rh_manager_config_commit("active/43") == CANOPUS_SVC_ERR_NOSPACE);
    open_error = 0;
    assert(closes == closed && renames == moved);
    assert_pointer("active/42\n");

    write_error = CANOPUS_SVC_ERR_NOSPACE;
    removed = removes;
    assert(rh_manager_config_commit("active/43") == CANOPUS_SVC_ERR_NOSPACE);
    write_error = 0;
    assert(closes == closed + 1u && removes == removed + 1u && renames == moved);
    assert_pointer("active/42\n");
    assert(!strcmp(rh_manager_config_current_dir(), ROOT "active/42/"));

    write_zero = 1;
    assert(rh_manager_config_commit("active/43") == CANOPUS_SVC_ERR_IO);
    write_zero = 0;
    write_oversize = 1;
    assert(rh_manager_config_commit("active/43") == CANOPUS_SVC_ERR_IO);
    write_oversize = 0;
    close_error = CANOPUS_SVC_ERR_IO;
    assert(rh_manager_config_commit("active/43") == CANOPUS_SVC_ERR_IO);
    close_error = 0;
    assert(renames == moved);
    assert_pointer("active/42\n");

    rename_error = CANOPUS_SVC_ERR_ACCESS;
    assert(rh_manager_config_commit("active/43") == CANOPUS_SVC_ERR_ACCESS);
    rename_error = 0;
    assert_pointer("active/42\n");
    assert(!strcmp(rh_manager_config_current_dir(), ROOT "active/42/"));
    recursive_commit = 1;
    assert(!rh_manager_config_commit("active/43"));
    assert_pointer("active/43\n");
    assert(!strcmp(rh_manager_config_current_dir(), ROOT "active/43/"));
}
static void bind_errors(const char *fault) {
    static const char *invalid[] = {
        "", "active/0\n", "active/01\n", "active/4294967296\n", "active/1", "active/1/\n",
        "active/1\r\n", "active/1\nactive/2\n", "active/1\n\n", "../active/1\n",
        "/active/1\n", "active/1/../../themes\n", "active/1111111111111111111111111111111111111\n"
    };
    static const char *bad_roots[] = {
        "", "/", "/data/canopus/m.other", "/data/canopus/m.corona//", "data/canopus/m.corona/",
        "/data/canopus/m.corona/../m.corona", "/data/canopus/m.coronaX"
    };
    size_t i;
    unsigned opened;
    struct canopus_context_v1 unsupported = context;
    assert(rh_manager_config_bind(NULL) == CANOPUS_SVC_ERR_UNSUPPORTED);
    unsupported.struct_size = 0;
    assert(rh_manager_config_bind(&unsupported) == CANOPUS_SVC_ERR_UNSUPPORTED);
    opened = pointer_opens;
    for (i = 0; i < sizeof(bad_roots) / sizeof(bad_roots[0]); ++i) {
        data_root = bad_roots[i];
        assert(rh_manager_config_bind(&context) == CANOPUS_SVC_ERR_ARGUMENT);
        assert(!rh_manager_config_current_dir());
    }
    assert(pointer_opens == opened);
    data_root = ROOT;
    root_error = CANOPUS_SVC_ERR_ACCESS;
    assert(rh_manager_config_bind(&context) == CANOPUS_SVC_ERR_ACCESS);
    root_error = 0;
    root_unterminated = 1;
    assert(rh_manager_config_bind(&context) == CANOPUS_SVC_ERR_ARGUMENT);
    root_unterminated = 0;

    set_pointer("active/42\n");
    if (!strcmp(fault, "embedded-nul")) {
        pointer_data[8] = 0; /* Embedded NUL cannot hide unparsed bytes. */
    } else if (!strcmp(fault, "oversized-read")) {
        read_oversize = 1;
    } else {
        char *end;
        unsigned long index = strtoul(fault, &end, 10);
        assert(*fault && !*end && index < sizeof(invalid) / sizeof(invalid[0]));
        set_pointer(invalid[index]);
    }
    {
        char out[256];
        int expected = read_oversize ? CANOPUS_SVC_ERR_IO : CANOPUS_SVC_ERR_ARGUMENT;
        assert(rh_manager_config_bind(&context) == expected);
        assert(!rh_manager_config_current_dir());
        opened = pointer_opens;
        /* Repair is permitted through the validated context even if reads
         * remain broken. The cached error never starts background retries. */
        assert(!rh_manager_config_candidate("active/42", out));
        assert(!strcmp(out, ROOT "active/42/"));
        assert(rh_manager_config_bind(&context) == expected);
        assert(pointer_opens == opened);
        assert(!rh_manager_config_commit("active/42"));
        assert(!rh_manager_config_bind(&context));
        assert(!strcmp(rh_manager_config_current_dir(), ROOT "active/42/"));
        assert(pointer_opens == opened);
    }
}
static int repair_pointer(const char *fault) {
    char out[256];
    unsigned opened;
    int expected = CANOPUS_SVC_ERR_ARGUMENT;
    set_pointer("active/42\n");
    if (!strcmp(fault, "malformed")) set_pointer("active/0\n");
    else if (!strcmp(fault, "read")) { read_error = CANOPUS_SVC_ERR_IO; expected = read_error; }
    else if (!strcmp(fault, "open")) { open_error = CANOPUS_SVC_ERR_ACCESS; expected = open_error; }
    else { assert(!strcmp(fault, "close")); close_error = CANOPUS_SVC_ERR_IO; expected = close_error; }
    assert(rh_manager_config_bind(&context) == expected);
    assert(!rh_manager_config_current_dir());
    assert(!rh_manager_config_candidate("active/43", out));
    assert(!strcmp(out, ROOT "active/43/"));
    opened = pointer_opens;
    assert(rh_manager_config_bind(&context) == expected);
    assert(pointer_opens == opened);
    open_error = close_error = 0;
    rename_error = CANOPUS_SVC_ERR_IO;
    assert(rh_manager_config_commit("active/43") == CANOPUS_SVC_ERR_IO);
    assert(!rh_manager_config_current_dir());
    assert(rh_manager_config_bind(&context) == expected);
    rename_error = 0;
    assert(!rh_manager_config_commit("active/43"));
    assert_pointer("active/43\n");
    assert(!strcmp(rh_manager_config_current_dir(), ROOT "active/43/"));
    assert(!rh_manager_config_bind(&context));
    assert(pointer_opens == opened);
    puts("manager config store: invalid startup pointer repaired by a new generation without rereading");
    return 0;
}
int main(int argc, char **argv) {
    char out[256];
    struct canopus_context_v1 other_context = context;
    unsigned opened;
    assert(!rh_manager_config_current_dir());
    assert(rh_manager_config_candidate("active/1", out) == CANOPUS_SVC_ERR_STATE);
    assert(rh_manager_config_commit("active/1") == CANOPUS_SVC_ERR_STATE);
    if (argc == 3 && !strcmp(argv[1], "repair")) return repair_pointer(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "errors")) bind_errors(argv[2]);
    else {
        if (argc > 1 && !strcmp(argv[1], "startup")) {
            set_pointer("active/4294967295\n");
            /* An incomplete temporary record is irrelevant to recovery. */
            memcpy(temporary_data, "active/99", 9);
            temporary_size = 9;
        } else if (argc > 1 && !strcmp(argv[1], "root-no-slash")) {
            data_root = "/data/canopus/m.corona";
        } else assert(argc == 1);
        assert(!rh_manager_config_bind(&context));
        assert(!strcmp(rh_manager_config_current_dir(), pointer_exists
            ? ROOT "active/4294967295/" : ROOT));
    }
    opened = pointer_opens;
    assert(!rh_manager_config_bind(&context));
    assert(pointer_opens == opened); /* Same-context bind performs no polling. */
    assert(rh_manager_config_bind(&other_context) == CANOPUS_SVC_ERR_STATE);
    invalid_candidates();
    commit_failures();
    puts("manager config store: startup pointer, canonical generations, short writes and failed publication passed");
    return 0;
}
