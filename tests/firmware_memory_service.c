/* Service callbacks are intercepted by the firmware emulation harness. */
#include "canopus_services.h"
#include "resource_hook_platform.h"

void *rh_test_mem_alloc(const struct canopus_context_v1 *ctx, uint32_t size) {
    (void)ctx;
    (void)size;
    return 0;
}
void rh_test_mem_free(const struct canopus_context_v1 *ctx, void *pointer) {
    (void)ctx;
    (void)pointer;
}
int rh_test_bind_mem(void) {
    static const struct canopus_services_v1 services = {
        .struct_size = sizeof(struct canopus_services_v1),
        .group_version = {[CANOPUS_SVC_MEM] = 1},
    };
    static const struct canopus_mem_obj mem = {
        .alloc = rh_test_mem_alloc, .free = rh_test_mem_free,
    };
    static const struct canopus_context_v1 ctx = {
        .struct_size = sizeof(struct canopus_context_v1),
        .services = &services, .mem = &mem,
    };
    return rh_platform_bind_mem(&ctx);
}
