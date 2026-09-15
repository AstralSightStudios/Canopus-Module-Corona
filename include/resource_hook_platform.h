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
void *rh_platform_refresh_timer_create(void (*callback)(void *));
void rh_platform_refresh_timer_delete(void *);
#endif
