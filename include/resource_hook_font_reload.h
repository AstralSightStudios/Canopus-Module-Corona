#ifndef RESOURCE_HOOK_FONT_RELOAD_H
#define RESOURCE_HOOK_FONT_RELOAD_H
#include "resource_hook.h"
/* UI timer owner ONLY, no IRQ lock. Experimental .155 standard serialized UI
 * path; NOT a GPU fault/recovery barrier. Immutable generation filenames are
 * required. 0 complete/no-op, 1 busy/retry, negative permanent refusal/failure.
 * changed is an output count of committed registry families (also on a
 * post-commit owner-refresh failure). New live fonts are never rolled back or
 * freed after publication. Must run before any legacy registry retargeting. */
int rh_font_reload(const struct rh_mapping_view *current, uint32_t *changed);
/* Irreversible for this module instance. Call BEFORE font work when restart or
 * teardown is observed. Does not dereference saved firmware pointers. */
void rh_font_reload_disable(void);
#endif
