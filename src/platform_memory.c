/* Select the existing exact-target allocator and critical-section adapter. */
#if defined(RH_TARGET_1043) && RH_TARGET_1043
#include "platform_band10pro.c"
#else
#include "platform_band11.c"
#endif
