#ifndef RESOURCE_HOOK_TARGET_P65_H
#define RESOURCE_HOOK_TARGET_P65_H

/* REDMI Watch 6 / p65 / 3.100.043, exact vela_ap.bin SHA-256:
 * a19b601477569765103eaf468b937bd8026b4d68ffa5c896bde222a79e475858.
 * Static recovery only; no device or production approval is implied.
 * See targets/redmi-watch-6-3.100.043/core-evidence.json.
 * SRAM functions below execute from the startup-copied 0x0037xxxx alias;
 * do not call their flash load bytes or substitute another target's addresses.
 */
#define RH_FW_POSIX_DRIVER             0x3c4501d0u
#define RH_FW_POSIX_CACHE_SIZE         0x3c4501d4u
#define RH_FW_POSIX_OPEN_SLOT          0x3c4501dcu
#define RH_FW_POSIX_OPEN               0x0c7357d5u
#define RH_FW_POSIX_EXPECT_CACHE_SIZE   0u

#define RH_FW_IMAGE_CACHE_SLOT         0x3c45012cu
#define RH_FW_HEADER_CACHE_SLOT        0x3c450130u
#define RH_FW_IMAGE_CACHE_CLASS        0x3c20c7f0u
#define RH_FW_HEADER_CACHE_CLASS       0x3c20c7c8u
/* void cache_drop(cache, key, user_data); user_data must explicitly be 0. */
#define RH_FW_CACHE_DROP               0x0c73baa9u
#define RH_FW_CACHE_RELEASE            0x0c73b99du
#define RH_FW_CACHE_LIST_SIZE_OFFSET   44u
#define RH_FW_CACHE_LIST_HEAD_OFFSET   48u

#define RH_FW_IMAGE_OBJECT_CLASS       0x3c2242a0u
#define RH_FW_IMAGE_SET_SRC            0x0c78acf1u
#define RH_FW_IMAGE_GET_INFO           0x0c71d195u
#define RH_FW_IMAGE_SOURCE_TYPE_OFFSET 92u
#define RH_FW_OBJECT_TREE_WALK         0x00380179u
#define RH_FW_OBJECT_STYLE_GET         0x0037d539u
#define RH_FW_OBJECT_STYLE_REFRESH     0x0037dd55u

#define RH_FW_DISPLAY_SLOT             0x3c450044u
#define RH_FW_INVALIDATE_AREA          0x00384399u
#define RH_FW_DISPLAY_WIDTH            0x00374fd9u
#define RH_FW_DISPLAY_HEIGHT           0x00375009u
#define RH_FW_DISPLAY_ACTIVE_SCREEN_OFFSET 688u

#define RH_CALENDAR_BACKGROUND "/resource/app/perpetual_calendar/launcher_icon.bin"
#define RH_CALENDAR_OUTPUT "/data/quickapp/app/perpetual_calendar/calendar_icon.bin"

#endif
