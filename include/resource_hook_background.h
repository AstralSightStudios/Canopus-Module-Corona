#ifndef RESOURCE_HOOK_BACKGROUND_H
#define RESOURCE_HOOK_BACKGROUND_H
#include "resource_hook.h"

/* Empty image selects the system background; otherwise it is a native path. */
struct rh_launcher_background { char image[RH_PATH]; };

/* Strict, newline-terminated format (keys must be in this order):
 *   version=1\nmode=system\n
 * or
 *   version=1\nmode=image\nimage=themes/<themeId>/<safe relative>\n
 * themeId is 1..64 lowercase ASCII letters, digits, '_' or '-'. Relative
 * components must be nonempty, not '.' or '..', and contain no NUL, ASCII
 * control, backslash or colon. The native root plus image must fit RH_PATH
 * including its terminator. No file/platform access is performed.
 * data need not be NUL-terminated; size is the exact byte count.
 * Returns 0 on success, -1 on invalid input; *out is unchanged on failure. */
int rh_launcher_background_parse(const char *data, uint32_t size,
                                  struct rh_launcher_background *out);
#endif
