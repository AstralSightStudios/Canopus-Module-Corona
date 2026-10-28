#ifndef RESOURCE_HOOK_CALENDAR_H
#define RESOURCE_HOOK_CALENDAR_H
#include "resource_hook.h"

#define RH_CALENDAR_CONFIG_PATH RH_APP_FILES_ROOT "calendar.cfg"
#if defined(RH_TARGET_P65) && RH_TARGET_P65
#define RH_CALENDAR_OUTPUT_PATH "/data/quickapp/app/perpetual_calendar/calendar_icon.bin"
#else
#define RH_CALENDAR_OUTPUT_PATH "/data/app/perpetual_calendar/calendar_icon.bin"
#endif
#define RH_CALENDAR_BANK0 RH_APP_FILES_ROOT "calendar-alpha-0.bin"
#define RH_CALENDAR_BANK1 RH_APP_FILES_ROOT "calendar-alpha-1.bin"
#define RH_CALENDAR_POOL_BYTES 12796u
#define RH_CALENDAR_CONFIG_BYTES 14336u
#define RH_CALENDAR_NONE 0u
#define RH_CALENDAR_TEXT 1u
#define RH_CALENDAR_IMAGE 2u

/* Item order is month, day, weekday. Context weekday order is Monday first.
 * Offsets point into the bounded pool; no borrowed config-file pointers. */
struct rh_calendar_item {
    uint32_t type, align;
    int32_t x, y;
    uint32_t size, color, color_native, count;
    uint16_t context[31];
};
struct rh_calendar_config {
    struct rh_calendar_item items[3];
    uint32_t pool_used;
    char pool[RH_CALENDAR_POOL_BYTES];
    uint32_t custom;
};
struct rh_calendar_date { int32_t year, month, day, weekday; uint32_t locale; };

static inline unsigned rh_calendar_context_count(unsigned item) {
    return item == 0 ? 12u : item == 1 ? 31u : item == 2 ? 7u : 0u;
}

void rh_calendar_defaults(struct rh_calendar_config *);
/* No heap or I/O; output must be staging memory, not the active config.
 * Missing file is handled by the owner as defaults, malformed file is rejected. */
int rh_calendar_parse(const char *, uint32_t, struct rh_calendar_config *);
const char *rh_calendar_context(const struct rh_calendar_config *, unsigned, unsigned);

#endif
