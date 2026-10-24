#ifndef CORONA_RUNTIME_H
#define CORONA_RUNTIME_H
#include "canopus_services.h"
/* Calls belong to the serialized module/UI owner. Start is idempotent. */
int rh_connect_runtime_start(const struct canopus_context_v1 *ctx);
void rh_connect_runtime_stop(void);
#endif
