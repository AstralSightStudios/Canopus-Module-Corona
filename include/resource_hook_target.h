#ifndef RESOURCE_HOOK_TARGET_H
#define RESOURCE_HOOK_TARGET_H

/* Exact AP-image addresses, not a version-range ABI. Callable addresses carry
 * the Thumb bit. Default remains 4.100.139; .155 requires RH_TARGET_155; 10 Pro
 * 3.101.043 requires RH_TARGET_1043 and the matching Canopus target headers.
 * Static evidence and hardware gates:
 * targets/xiaomi-band-11-4.100.139/ui-reload-audit.md,
 * targets/xiaomi-band-11-4.100.155/ui-reload-audit.md, and
 * targets/xiaomi-band-10-pro-3.101.043/ui-reload-audit.md. */
#if defined(RH_TARGET_1043) && RH_TARGET_1043
/* Exact Xiaomi Band 10 Pro 3.101.043 identities. */
#define RH_CALENDAR_BACKGROUND "/resource/app/perpetual_calendar/calendar_background_icon.bin"
#define RH_CALENDAR_OUTPUT "/resource/app/perpetual_calendar/launcher.bin"
#define RH_CALENDAR_APP_ID "com.xiaomi.miwear.perpetual_calendar"
#define RH_FW_CALENDAR_LOOKUP         0x0ca69935u
#define RH_FW_CALENDAR_LOOKUP_OTHER   0x0ca6996du
#define RH_FW_CALENDAR_SIGNAL         0x0c4efde9u
#define RH_FW_CALENDAR_NOTIFY         0x0ca6a005u
#define RH_FW_CALENDAR_VTABLE_SLOT    0x200eb658u
#define RH_FW_CALENDAR_VTABLE         0x2cdbb054u
#define RH_FW_CALENDAR_LOOKUP_NAME    0x0ca69e81u
#define RH_FW_CALENDAR_LOOKUP_NAME_OTHER 0x0ca69e55u
#define RH_FW_CALENDAR_DISPATCH       0x0ca69aa1u
#define RH_FW_IMAGE_OBJECT_CLASS      0x2cce61ecu
#define RH_FW_CACHE_DROP              0x0c1667ddu
#define RH_FW_IMAGE_CACHE_CLASS       0x2cce56f4u
#define RH_FW_HEADER_CACHE_CLASS      0x2cce571cu
#define RH_FW_FONT_REMOVE_PATH        0x0c860399u
#define RH_FW_CACHE_RELEASE           0x0c1666edu

#define RH_FW_IMAGE_SET_SRC           0x0c17a2f5u
#define RH_FW_IMAGE_GET_INFO          0x0c143799u
#define RH_FW_OBJECT_TREE_WALK        0x0c13c691u
#define RH_FW_OBJECT_STYLE_GET        0x0c1068a9u
#define RH_FW_OBJECT_STYLE_REFRESH    0x0c1070adu

/* nx_open returns -errno instead of setting task errno. */
#define RH_FW_OPEN_NEGATIVE_ERRNO     1
#define RH_FW_OPEN                    0x0c1d0a29u
#define RH_FW_READ                    0x0c1d129du
#define RH_FW_WRITE                   0x0c1d2641u
#define RH_FW_CLOSE                   0x0c1b9d81u
#define RH_FW_ERRNO_LOCATION          0x0c1e45bdu
#define RH_FW_POSIX_DRIVER            0x20103340u
#define RH_FW_POSIX_CACHE_SIZE        0x20103344u
#define RH_FW_POSIX_OPEN_SLOT         0x2010334cu
#define RH_FW_POSIX_OPEN              0x1c057c51u
#define RH_FW_IMAGE_CACHE_SLOT        0x2010329cu
#define RH_FW_HEADER_CACHE_SLOT       0x201032a0u
#define RH_FW_DISPLAY_SLOT            0x2010318cu
#define RH_FW_INVALIDATE_AREA         0x0c105165u
#define RH_FW_DISPLAY_WIDTH           0x0c13cd55u
#define RH_FW_DISPLAY_HEIGHT          0x0c13cd75u
#define RH_FW_UIKIT_SLOT              0x20103174u
#define RH_FW_UIKIT_MANAGER_OFFSET    28u
#define RH_FW_MANAGER_PATH_LIST_HEAD_OFFSET 28u
#define RH_FW_FONT_ADD_PATH           0x0c86037du
#define RH_FW_TIMER_CREATE            0x0c587ed1u
#define RH_FW_TIMER_DELETE            0x0c588129u
#define RH_FW_MALLOC                  0x0c1f903du
#define RH_FW_FREE                    0x0c1f8ff9u
#elif defined(RH_TARGET_155) && RH_TARGET_155
/* Exact .155 cache and LVGL image-class identities. */
#define RH_FW_IMAGE_OBJECT_CLASS  0x2ca14ca8u
#define RH_FW_CACHE_DROP          0x0c8b8c9fu
#define RH_FW_IMAGE_CACHE_CLASS   0x2ca168b4u
#define RH_FW_HEADER_CACHE_CLASS  0x2ca16934u
#define RH_FW_FONT_REMOVE_PATH    0x0c904cedu
/* Font transaction identities: individually mapped, not a range delta. */
#define RH_FW_VECTOR_CLASS            0x2ca6ee48u
#define RH_FW_VECTOR_COMPARE          0x0c69feb1u
#define RH_FW_VECTOR_DESTROY          0x0c6a12d5u
#define RH_FW_VECTOR_DROP             0x0c6a1305u
#define RH_FW_FONT_SET_PIXEL_SIZE     0x0c8b8a55u
#define RH_FW_CACHE_RELEASE           0x0c8b9781u
#else
/* Exact .139 cache and LVGL image-class identities, verified in its AP. */
#define RH_FW_IMAGE_OBJECT_CLASS  0x2ca14cb8u
#define RH_FW_CACHE_DROP          0x0c8b8cafu
#define RH_FW_IMAGE_CACHE_CLASS   0x2ca168c4u
#define RH_FW_HEADER_CACHE_CLASS  0x2ca16944u
#define RH_FW_FONT_REMOVE_PATH    0x0c904cfdu
/* Font transaction identities: individually mapped, not a range delta. */
#define RH_FW_VECTOR_CLASS            0x2ca6ee58u
#define RH_FW_VECTOR_COMPARE          0x0c69fec1u
#define RH_FW_VECTOR_DESTROY          0x0c6a12e5u
#define RH_FW_VECTOR_DROP             0x0c6a1315u
#define RH_FW_FONT_SET_PIXEL_SIZE     0x0c8b8a65u
#define RH_FW_CACHE_RELEASE           0x0c8b9791u
#endif

#if !(defined(RH_TARGET_1043) && RH_TARGET_1043)
/* Native launcher calendar generation/publication have the same entries in
 * both exact APs; snapshot leaves/literals are individually target-specific. */
#define RH_CALENDAR_BACKGROUND "/resource/app/perpetual_calendar/launcher_icon.bin"
#define RH_CALENDAR_OUTPUT "/data/app/perpetual_calendar/calendar_icon.bin"
#define RH_FW_CALENDAR_GENERATE       0x0c5487a5u
#define RH_FW_CALENDAR_PUBLISH        0x0c5486d1u
#define RH_FW_CALENDAR_LAUNCHER_SLOT  0x200c6010u
/* Get-info, tree walk and style APIs are byte-identical at these Thumb
 * addresses in both exact APs. The setter entry is also shared but its .139
 * control flow is independently audited. The reload probe executes both APs. */
#define RH_FW_IMAGE_SET_SRC       0x0c3b2c29u
#define RH_FW_IMAGE_GET_INFO      0x0c38e4e5u
#define RH_FW_OBJECT_TREE_WALK    0x0c380575u
#define RH_FW_OBJECT_STYLE_GET    0x0c382621u
#define RH_FW_OBJECT_STYLE_REFRESH 0x0c38525du

/* Independently checked in the .155 AP; unchanged from .139. */
/* POSIX open returns -1 and sets task errno. */
#define RH_FW_OPEN_NEGATIVE_ERRNO 0
#define RH_FW_OPEN                0x0c342c55u
#define RH_FW_READ                0x0c33d785u
#define RH_FW_WRITE               0x0c33dc4fu
#define RH_FW_CLOSE               0x0c33818du
#define RH_FW_POSIX_DRIVER        0x200bd3b8u
#define RH_FW_POSIX_CACHE_SIZE    0x200bd3bcu
#define RH_FW_POSIX_OPEN_SLOT     0x200bd3c4u
#define RH_FW_POSIX_OPEN          0x0c3a6195u
#define RH_FW_IMAGE_CACHE_SLOT    0x200bd310u
#define RH_FW_HEADER_CACHE_SLOT   0x200bd314u
#define RH_FW_DISPLAY_SLOT        0x200bd200u
#define RH_FW_INVALIDATE_AREA     0x0c382429u
#define RH_FW_DISPLAY_WIDTH       0x0c380695u
#define RH_FW_DISPLAY_HEIGHT      0x0c3806b5u
#define RH_FW_UIKIT_SLOT          0x200bd1e8u
#define RH_FW_UIKIT_MANAGER_OFFSET 20u
#define RH_FW_MANAGER_PATH_LIST_HEAD_OFFSET 4u
#define RH_FW_FONT_ADD_PATH       0x0c4924e1u
#define RH_FW_FONT_RESOLVE_PATH   0x0c490eddu
#define RH_FW_TIMER_CREATE        0x0c3abd21u
#define RH_FW_TIMER_DELETE        0x0c3abe71u
#endif

#endif
