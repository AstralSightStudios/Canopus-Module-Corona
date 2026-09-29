#include "resource_hook_platform.h"
#include "resource_hook_target.h"

#if defined(RH_TARGET_1043) && RH_TARGET_1043
#error "Band 11 native platform cannot be used for Band 10 Pro"
#endif
#include "canopus_band11_memory.h"

void *rh_platform_alloc(uint32_t size) { return b11_temp_alloc(8u, size); }
void rh_platform_free(void *p) { b11_temp_free(p); }
uint32_t rh_platform_lock(void) { return b11_irq_lock(); }
void rh_platform_unlock(uint32_t irq) {
    b11_barrier();
    b11_irq_unlock(irq);
}
