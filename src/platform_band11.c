#include "resource_hook_platform.h"
#include "resource_hook_target.h"

#if defined(RH_TARGET_1043) && RH_TARGET_1043
#error "Band 11 native platform cannot be used for Band 10 Pro"
#endif
uint32_t rh_platform_lock(void) {
    uint32_t irq;
    __asm__ volatile("mrs %0, primask\ncpsid i" : "=r"(irq) :: "memory");
    return irq;
}
void rh_platform_unlock(uint32_t irq) {
    __asm__ volatile("dsb sy\nisb sy" ::: "memory");
    __asm__ volatile("msr primask, %0" :: "r"(irq) : "memory");
}
