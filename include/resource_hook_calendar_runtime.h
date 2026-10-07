#ifndef RESOURCE_HOOK_CALENDAR_RUNTIME_H
#define RESOURCE_HOOK_CALENDAR_RUNTIME_H
#include "resource_hook_calendar.h"
/* Configuration staging is non-graphics work. A successful commit transfers
 * ownership; all render/poll/refresh operations require the serialized UI owner. */
int rh_calendar_stage(struct rh_calendar_config **);
int rh_calendar_commit(struct rh_calendar_config *);
void rh_calendar_discard(struct rh_calendar_config *);
int rh_calendar_start(void);
int rh_calendar_refresh(void);
int rh_calendar_poll(void);
int rh_calendar_pending(void);
/* Read hook only: no clock/config reads, allocation or rendering. Zero means
 * no ready derived file; caller continues the ordinary mapping/fallback path. */
const char *rh_calendar_redirect(const char *);
#endif
