#ifndef RESOURCE_HOOK_BACKGROUND_H
#define RESOURCE_HOOK_BACKGROUND_H
#include "resource_hook.h"

/* Empty image selects the system background; otherwise it is a native path. */
struct rh_backgrounds {
    char launcher[RH_PATH];
    char control_center[RH_PATH];
    char notifications[RH_PATH];
};

struct rh_launcher_background { char image[RH_PATH]; };

/* Strict newline-terminated format:
 *   version=1\nlauncher=themes/<id>/<path>\ncontrol_center=system\nnotifications=...\n
 * Also accepts legacy format:
 *   version=1\nmode=system\n
 *   version=1\nmode=image\nimage=themes/<id>/<path>\n
 * Returns 0 on success, -1 on invalid input; *out is unchanged on failure. */
int rh_backgrounds_parse(const char *data, uint32_t size,
                         struct rh_backgrounds *out);

int rh_launcher_background_parse(const char *data, uint32_t size,
                                 struct rh_launcher_background *out);
#endif
