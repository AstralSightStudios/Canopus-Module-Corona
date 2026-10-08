#ifndef RESOURCE_HOOK_CALENDAR_NATIVE_H
#define RESOURCE_HOOK_CALENDAR_NATIVE_H
#include "resource_hook_calendar.h"

/* Capability is compile-target based (exact Band 11 targets); receipt mismatch fails the
 * date/render operation rather than silently disabling configured overrides.
 * Other builds return unsupported/-1 without native calls.
 * UI owner, serialized with LVGL, outside the IRQ lock. output is an inactive
 * immutable generation bank, never the currently published filename.
 * 0: complete file; -1: refusal/failure, caller must retain old publication.
 * A failed GPU fence retains the native tree/fonts/buffer until a subsequent
 * render obtains a successful fence. Do not unload/restart with pending work.
 * This is healthy-GPU completion, not GPU fault/recovery safety. Platform
 * close has no status return, so durability/close errors cannot be certified. */
int rh_calendar_native_supported(void);
int rh_calendar_native_date(struct rh_calendar_date *);
int rh_calendar_native_render(const struct rh_calendar_config *,
                              const struct rh_calendar_date *, const char *output);
#endif
