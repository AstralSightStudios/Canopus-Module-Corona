#ifndef RESOURCE_HOOK_PLATFORM_H
#define RESOURCE_HOOK_PLATFORM_H
#include "resource_hook.h"
#include <stdint.h>

int rh_platform_open(const char *, int);
int rh_platform_read(int, void *, uint32_t);
int rh_platform_write(int, const void *, uint32_t);
void rh_platform_close(int);
void *rh_platform_alloc(uint32_t);
void rh_platform_free(void *);
void *rh_platform_driver(void);
rh_open_fn *rh_platform_slot(void);
rh_open_fn rh_platform_original(void);
int rh_platform_driver_valid(void);
uint32_t rh_platform_lock(void);
void rh_platform_unlock(uint32_t);
/* All graphics operations require the UI owner thread, outside the IRQ lock.
 * A repaint is not a resource rebuild; retained decoders/fonts are not replaced. */
int rh_platform_image_cache_drop_all(void);
int rh_platform_redraw_ready(void);
/* 0 only when a full-display dirty area was retained; otherwise retry later. */
int rh_platform_request_full_redraw(void);
/* Destroy and recreate the stack-top page so widgets holding a resource are
 * rebuilt, not just repainted. 0 on a completed rebuild; -1 if it was skipped
 * (no page, screen off, page layer inactive, page not resumed, or its destroy
 * would be deferred) or did not come back up. A rebuild that starts and then
 * fails leaves that page torn down. One page only; the rest of the stack is
 * untouched. */
int rh_platform_rebuild_active_page(void);
/* The font manager's registered-path registry, which is how a font family name
 * becomes a native file path. Fonts never pass through the hooked LVGL POSIX
 * open, so retargeting this registry is the only way a theme replaces a font.
 * Both take an index into the registry; get() fills RH_PATH-sized buffers and
 * returns -1 past the end. retarget() removes and re-adds the entry, so it moves
 * to the end of the registry and every index at or after it shifts down; it
 * confirms the manager resolves the family to `path` before reporting success.
 * Only new font wrappers are affected: live ones must be rebuilt with their page. */
int rh_platform_font_path_get(uint32_t index, char *name, char *path);
int rh_platform_font_retarget(uint32_t index, const char *path);
void *rh_platform_refresh_timer_create(void (*callback)(void *));
void rh_platform_refresh_timer_delete(void *);
#endif
