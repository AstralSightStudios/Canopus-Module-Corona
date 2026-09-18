#ifndef RESOURCE_HOOK_TARGET_H
#define RESOURCE_HOOK_TARGET_H

/* Exact AP-image addresses, not a version-range ABI. Callable addresses carry
 * the Thumb bit. Default remains 4.100.139; .155 requires RH_TARGET_155 and the
 * matching Canopus generated SDK headers. Static evidence and hardware gates:
 * targets/xiaomi-band-11-4.100.155/ui-reload-audit.md. */
#if defined(RH_TARGET_155) && RH_TARGET_155
#define RH_FW_CACHE_DROP          0x0c8b8c9fu
#define RH_FW_IMAGE_CACHE_CLASS   0x2ca168b4u
#define RH_FW_HEADER_CACHE_CLASS  0x2ca16934u
#define RH_FW_PAGE_POP            0x0c696e25u
#define RH_FW_PAGE_RESUME         0x0c696c09u
#define RH_FW_PAGE_TOP            0x0c697441u
#define RH_FW_FONT_REMOVE_PATH    0x0c904cedu
#else
#define RH_FW_CACHE_DROP          0x0c8b8cafu
#define RH_FW_IMAGE_CACHE_CLASS   0x2ca168c4u
#define RH_FW_HEADER_CACHE_CLASS  0x2ca16944u
#define RH_FW_PAGE_POP            0x0c696e35u
#define RH_FW_PAGE_RESUME         0x0c696c19u
#define RH_FW_PAGE_TOP            0x0c697451u
#define RH_FW_FONT_REMOVE_PATH    0x0c904cfdu
#endif

/* Independently checked in the .155 AP; unchanged from .139. */
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
#define RH_FW_SCREEN_STATE        0x200c2a28u
#define RH_FW_PAGE_LAYER_ACTIVE   0x20096085u
#define RH_FW_UIKIT_SLOT          0x200bd1e8u
#define RH_FW_FONT_ADD_PATH       0x0c4924e1u
#define RH_FW_FONT_RESOLVE_PATH   0x0c490eddu
#define RH_FW_TIMER_CREATE        0x0c3abd21u
#define RH_FW_TIMER_DELETE        0x0c3abe71u

#endif
