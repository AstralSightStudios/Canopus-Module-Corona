#include "resource_hook_platform.h"
#include "canopus_services.h"

static const struct canopus_context_v1 *mem_context;

int rh_platform_bind_mem(const struct canopus_context_v1 *ctx) {
    if (!canopus_services_has(ctx, CANOPUS_SVC_MEM, 1) || !ctx->mem ||
        !ctx->mem->alloc || !ctx->mem->free) return CANOPUS_SVC_ERR_UNSUPPORTED;
    if (mem_context && mem_context != ctx) return CANOPUS_SVC_ERR_STATE;
    mem_context = ctx;
    return 0;
}

/* Allocation and release belong to the serialized module/UI owner. The open
 * hook queues its last snapshot reference for owner-side collection instead. */
void *rh_platform_alloc(uint32_t size) {
    return mem_context ? mem_context->mem->alloc(mem_context, size) : 0;
}
void rh_platform_free(void *pointer) {
    if (mem_context && pointer) mem_context->mem->free(mem_context, pointer);
}

#ifndef CANOPUS_HOST
/* Exact-target adapters now supply only the ARM critical section. */
#if defined(RH_TARGET_1043) && RH_TARGET_1043
#include "platform_band10pro.c"
#else
#include "platform_band11.c"
#endif
#endif
