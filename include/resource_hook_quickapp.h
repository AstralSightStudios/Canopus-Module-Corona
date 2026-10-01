#ifndef RESOURCE_HOOK_QUICKAPP_H
#define RESOURCE_HOOK_QUICKAPP_H

#include "resource_hook.h"

/* UI-owner task only, outside IRQ/manager locks. package is an opaque NUL-ended
 * registry key (empty allowed), <=255 bytes, without ASCII controls/DEL, and
 * must not overlap out. This read-only adapter neither builds
 * icons nor opens files; a success is a registered BIN pathname, not proof of
 * file existence/decodability. All borrowed firmware strings are copied during
 * the call; no native pointers escape or survive it.
 *  1: exact app's registered canonical BIN pathname beneath /data/app/ on
 *     10 Pro .043, or /data/quickapp/app/ on Band 11 .139/.155;
 *     package identity is verified, but package-directory ownership is not
 *  0: absent icon/app or uninitialized service/registry
 * -1: invalid/transient native record or registry; retry on a later owner turn
 * -2: unsupported exact target or source type/format (including PNG)
 * out is empty on every non-success (when non-null).
 */
int rh_platform_quickapp_icon_path(const char *package, char out[RH_PATH]);

#endif
