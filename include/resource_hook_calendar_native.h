#ifndef RESOURCE_HOOK_CALENDAR_NATIVE_H
#define RESOURCE_HOOK_CALENDAR_NATIVE_H
#include "resource_hook_calendar.h"

/* All supported exact targets implement this adapter. Receipt mismatch fails
 * date/render rather than silently disabling configured overrides.
 * Non-ARM host stubs return -1 without native calls.
 * UI owner, serialized with LVGL, outside the IRQ lock. Band 11 output is an
 * inactive immutable file bank, never the currently published filename.
 * P67 ignores output (pass NULL) and prepares the inactive RAM draw buffer.
 * 0: complete file/prepared RAM; -1: failure, retain the old publication.
 * A failed GPU fence retains the native tree/fonts/buffer until a subsequent
 * render obtains a successful fence. Do not unload/restart with pending work.
 * This is healthy-GPU completion, not GPU fault/recovery safety. Platform
 * close has no status return, so durability/close errors cannot be certified. */
int rh_calendar_native_date(struct rh_calendar_date *);
int rh_calendar_native_render(const struct rh_calendar_config *,
                              const struct rh_calendar_date *, const char *output);
#if defined(RH_TARGET_1043) && RH_TARGET_1043
/* Owner-only RAM transaction: publish after both cache retirements, cancel a
 * prepared generation if the second retirement fails. Reset restores stock
 * only after a successful GPU fence; -1 retains the previous publication. */
void rh_p67_calendar_publish(void);
void rh_p67_calendar_cancel(void);
int rh_p67_calendar_reset(void);
#endif
#endif
