#include "resource_hook_background.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char image_prefix[] = "version=1\nmode=image\nimage=";

static void rejected_bytes(const char *data, uint32_t size) {
    struct rh_launcher_background out, saved;
    memset(&out, 0xa5, sizeof(out));
    saved = out;
    assert(rh_launcher_background_parse(data, size, &out) < 0);
    assert(!memcmp(&out, &saved, sizeof(out)));
}

static void rejected(const char *data) {
    rejected_bytes(data, (uint32_t)strlen(data));
}

static void image_case(const char *image, int valid) {
    char text[RH_PATH * 2];
    struct rh_launcher_background out;
    int n = snprintf(text, sizeof(text), "%s%s\n", image_prefix, image);
    assert(n > 0 && (size_t)n < sizeof(text));
    if (!valid) {
        rejected_bytes(text, (uint32_t)n);
        return;
    }
    memset(&out, 0xa5, sizeof(out));
    assert(!rh_launcher_background_parse(text, (uint32_t)n, &out));
    assert(!strncmp(out.image, RH_APP_FILES_ROOT, strlen(RH_APP_FILES_ROOT)));
    assert(!strcmp(out.image + strlen(RH_APP_FILES_ROOT), image));
}

static void test_format(void) {
    static const char system_config[] = "version=1\nmode=system\n";
    static const char image_config[] =
        "version=1\nmode=image\nimage=themes/base/background.bin\n";
    static const char *const bad[] = {
        "", "\n", "version=1\n", "mode=system\n",
        "version=2\nmode=system\n", "version=01\nmode=system\n",
        "version=1\nmode=unknown\n", "version=1\nmode=System\n",
        "mode=system\nversion=1\n", "version=1\nmode=system",
        "version=1\r\nmode=system\r\n", "version=1\nmode=system\n\n",
        " version=1\nmode=system\n", "version =1\nmode=system\n",
        "version=1\nmode=system \n", "# comment\nversion=1\nmode=system\n",
        "version=1\nversion=1\nmode=system\n",
        "version=1\nmode=system\nmode=system\n",
        "version=1\nmode=system\nimage=themes/base/a.bin\n",
        "version=1\nmode=image\n", "version=1\nmode=image\nimage=\n",
        "version=1\nimage=themes/base/a.bin\nmode=image\n",
        "version=1\nmode=image\nimage=themes/base/a.bin",
        "version=1\nmode=image\nimage=themes/base/a.bin\r\n",
        "version=1\nmode=image\nimage=themes/base/a.bin\nextra=1\n",
        "version=1\nmode=image\nimage=themes/base/a.bin\nimage=themes/base/b.bin\n"
    };
    struct rh_launcher_background out;
    char exact[sizeof(image_config) - 1u], changed[sizeof(image_config)];
    uint32_t i;

    memset(&out, 0xa5, sizeof(out));
    assert(!rh_launcher_background_parse(system_config,
                                         sizeof(system_config) - 1u, &out));
    assert(out.image[0] == 0);
    for (i = 1; i < sizeof(out.image); i++)
        assert((unsigned char)out.image[i] == 0xa5);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) rejected(bad[i]);
    rejected_bytes(NULL, 0);
    rejected_bytes(NULL, 1);
    assert(rh_launcher_background_parse(system_config,
                                        sizeof(system_config) - 1u, NULL) < 0);
    /* Reject a hostile size without accessing that many caller bytes. */
    rejected_bytes("x", UINT32_MAX);
    rejected_bytes(system_config, sizeof(system_config));
    rejected_bytes(image_config, sizeof(image_config));
    for (i = 0; i < sizeof(system_config) - 1u; i++)
        rejected_bytes(system_config, i);
    for (i = 0; i < sizeof(image_config) - 1u; i++) {
        rejected_bytes(image_config, i);
        memcpy(changed, image_config, sizeof(image_config));
        changed[i] = 0;
        rejected_bytes(changed, sizeof(image_config) - 1u);
    }
    for (i = 0; i < sizeof(system_config) - 1u; i++) {
        memcpy(changed, system_config, sizeof(system_config));
        changed[i] = 0;
        rejected_bytes(changed, sizeof(system_config) - 1u);
    }
    /* No terminator is needed or examined outside the declared byte range. */
    memcpy(exact, image_config, sizeof(exact));
    assert(!rh_launcher_background_parse(exact, sizeof(exact), &out));
    assert(!strcmp(out.image, RH_THEME_ROOT "base/background.bin"));
    memcpy(changed, image_config, sizeof(image_config));
    changed[sizeof(image_config) - 1u] = '!';
    assert(!rh_launcher_background_parse(changed, sizeof(image_config) - 1u, &out));
}

static void test_paths(void) {
    static const char *const bad[] = {
        "/themes/base/a.bin", "themes-evil/base/a.bin", "theme/base/a.bin",
        "themes/", "themes/base", "themes/base/", "themes//a.bin",
        "themes/Base/a.bin", "themes/a.b/a.bin", "themes/./a.bin",
        "themes/../a.bin", "themes/a b/a.bin", "themes/a:b/a.bin",
        "themes/a\\b/a.bin", "themes/base//a.bin", "themes/base/a//b.bin",
        "themes/base/.", "themes/base/..", "themes/base/./a.bin",
        "themes/base/../a.bin", "themes/base/a/../b.bin",
        "themes/base/a/./b.bin", "themes/base/a.bin/",
        "themes/base/a\\b.bin", "themes/base/a:b.bin",
        "themes/base/C:/a.bin", "themes/base/a.bin\nb.bin"
    };
    char path[RH_PATH * 2];
    uint32_t i, maximum = RH_PATH - 1u - (uint32_t)strlen(RH_APP_FILES_ROOT);

    image_case("themes/a/a.bin", 1);
    image_case("themes/base-1_2/icons/nested/background.bin", 1);
    image_case("themes/0/.hidden/.../a..bin", 1);
    image_case("themes/_-/x.bin", 1);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) image_case(bad[i], 0);
    for (i = 1; i <= 127u; i++) {
        if (i >= 32u && i != 127u) continue;
        strcpy(path, "themes/base/axb.bin");
        path[13] = (char)i;
        image_case(path, 0);
        strcpy(path, "themes/axb/file.bin");
        path[8] = (char)i;
        image_case(path, 0);
    }
    strcpy(path, "themes/");
    memset(path + 7, 'a', 64);
    strcpy(path + 71, "/file.bin");
    image_case(path, 1);
    memset(path + 7, 'a', 65);
    strcpy(path + 72, "/file.bin");
    image_case(path, 0);
    strcpy(path, "themes/base/");
    assert(maximum > strlen(path));
    memset(path + strlen(path), 'x', maximum - strlen(path));
    path[maximum] = 0;
    image_case(path, 1); /* Native path is RH_PATH-1 bytes plus NUL. */
    path[maximum] = 'x';
    path[maximum + 1u] = 0;
    image_case(path, 0); /* One byte too long after native-root prefixing. */
    memset(path, 'x', sizeof(path) - 1u);
    path[sizeof(path) - 1u] = 0;
    rejected_bytes(path, sizeof(path) - 1u);
}

static void test_multi_background(void) {
    static const char multi_all[] =
        "version=1\n"
        "launcher=themes/a/l.bin\n"
        "control_center=themes/b/c.bin\n"
        "notifications=themes/c/n.bin\n";
    static const char multi_partial[] =
        "version=1\n"
        "control_center=themes/b/c.bin\n"
        "launcher=system\n";
    static const char multi_blur[] =
        "version=1\n"
        "launcher=blur\n"
        "control_center=blur\n"
        "notifications=blur\n";
    struct rh_backgrounds out;
    memset(&out, 0xa5, sizeof(out));
    assert(!rh_backgrounds_parse(multi_all, (uint32_t)strlen(multi_all), &out));
    assert(!strcmp(out.launcher + strlen(RH_APP_FILES_ROOT), "themes/a/l.bin"));
    assert(!strcmp(out.control_center + strlen(RH_APP_FILES_ROOT), "themes/b/c.bin"));
    assert(!strcmp(out.notifications + strlen(RH_APP_FILES_ROOT), "themes/c/n.bin"));

    memset(&out, 0xa5, sizeof(out));
    assert(!rh_backgrounds_parse(multi_partial, (uint32_t)strlen(multi_partial), &out));
    assert(out.launcher[0] == 0);
    assert(!strcmp(out.control_center + strlen(RH_APP_FILES_ROOT), "themes/b/c.bin"));
    assert(out.notifications[0] == 0);

    memset(&out, 0xa5, sizeof(out));
    assert(!rh_backgrounds_parse(multi_blur, (uint32_t)strlen(multi_blur), &out));
    assert(rh_background_is_blur(out.launcher));
    assert(rh_background_is_blur(out.control_center));
    assert(rh_background_is_blur(out.notifications));

    /* Invalid key rejected */
    assert(rh_backgrounds_parse("version=1\nunknown=themes/a/x.bin\n", 31, &out) < 0);
}

int main(void) {
    test_format();
    test_paths();
    test_multi_background();
    puts("launcher background bounded parser tests passed");
    return 0;
}
