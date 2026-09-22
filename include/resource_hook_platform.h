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
/* 0 = completed, 1 = unsupported target (no mutation), -1 = retry.
 * The immutable published mapping snapshot is shared with the open hook.
 * Retirement only unlinks matching file keys; native refcounts own payloads. */
int rh_platform_retire_images(const struct rh_state *);
/* After retirement: refresh exact image-class file metadata and supported
 * effective image-bearing styles. Unsupported owner types are left alone. */
int rh_platform_refresh_images(const struct rh_state *);
int rh_platform_redraw_ready(void);
/* 0 only when a full-display dirty area was retained; otherwise retry later. */
int rh_platform_request_full_redraw(void);
/* The font manager's registered-path registry, which is how a font family name
 * becomes a native file path. Fonts never pass through the hooked LVGL POSIX
 * open, so retargeting this registry is the only way a theme replaces a font.
 * Both take an index into the registry; get() fills RH_PATH-sized buffers and
 * returns -1 past the end. retarget() removes and re-adds the entry, so it moves
 * to the end of the registry and every index at or after it shifts down; it
 * confirms the manager resolves the family to `path` before reporting success.
 * Only future registry resolutions are affected; active/idle wrapper reuse may
 * bypass them. */
int rh_platform_font_path_get(uint32_t index, char *name, char *path);
int rh_platform_font_retarget(uint32_t index, const char *path);
void *rh_platform_refresh_timer_create(void (*callback)(void *));
void rh_platform_refresh_timer_delete(void *);
#endif
