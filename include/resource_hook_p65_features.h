#ifndef RESOURCE_HOOK_P65_FEATURES_H
#define RESOURCE_HOOK_P65_FEATURES_H

/* Exact P65 3.100.043 only. Static AP evidence and native instruction probes:
 * targets/redmi-watch-6-3.100.043/features-audit.{md,json}.
 * These are private adapter identities, never device/production approvals. */
#define RH_P65_QA_SERVICE_SLOT 0x3c3cf934u
#define RH_P65_QA_REGISTRY_SLOT 0x3c3cf92cu
#define RH_P65_QA_SERVICE 0x3c350b30u
#define RH_P65_QA_LOOKUP 0x1c15165du
#define RH_P65_QA_PACKAGE_OFFSET 8u
#define RH_P65_QA_ICON_OFFSET 12u
#define RH_P65_QA_APP_ROOT "/data/quickapp/app/"
#define RH_P65_QA_FLASH_END 0x0cbff484u

/* Managed Manager label font lifecycle is bound by the framework; it does not
 * establish the module's in-place font transaction or native calendar ABI. */
#define RH_P65_NATIVE_FONT_CREATE 0x0c795db9u
#define RH_P65_NATIVE_FONT_RELEASE 0x0c795fe5u
#define RH_P65_NATIVE_FONT_WAIT 0x0c72ef2du

#endif
