#include "resource_hook_platform.h"
#include "resource_hook_target.h"

#if !(defined(RH_TARGET_1043) && RH_TARGET_1043)
#error "Band 10 Pro native platform requires RH_TARGET_1043"
#endif

void *rh_platform_alloc(uint32_t size) {
    return ((void *(*)(uint32_t))(uintptr_t)RH_FW_MALLOC)(size);
}
void rh_platform_free(void *p) {
    if (p) {
        ((void (*)(void *))(uintptr_t)RH_FW_FREE)(p);
    }
}
uint32_t rh_platform_lock(void) {
    uint32_t p;
    __asm__ volatile("mrs %0, primask\ncpsid i" : "=r"(p) :: "memory");
    return p;
}
void rh_platform_unlock(uint32_t irq) {
    __asm__ volatile("dsb sy\nisb sy" ::: "memory");
    __asm__ volatile("msr primask, %0" :: "r"(irq) : "memory");
}
