#ifndef RESOURCE_HOOK_Q66_179_H
#define RESOURCE_HOOK_Q66_179_H

/* Exact Xiaomi Band 11 4.100.179 AP identities.
 * AP SHA256: 307adabe0ff61c77be73d045a8260e279cddd131c7a535a2678eb34199278a2b.
 * See targets/xiaomi-band-11-4.100.179/address-map.json and README.md.
 * STATIC_RECOVERED; physical device NOT_PROBED. Framework restricted APIs
 * retain their existing policy and approval states.
 * RH_CN function constants are even entries; N() adds the Thumb bit.
 * Receipt constants are read from the exact AP, including GENERATE + 4.
 */

#define RH_CN_CLOCK 0x0c34d45cu
#define RH_CN_LOCALTIME 0x0c34fcecu
#define RH_CN_ROOT_CREATE 0x0c382ff0u
#define RH_CN_SIZE 0x0c3836d2u
#define RH_CN_ROOT_STYLE 0x0c502de8u
#define RH_CN_IMAGE_CREATE 0x0c3ae138u
#define RH_CN_IMAGE_SOURCE 0x0c3b2b90u
#define RH_CN_IMAGE_INFO 0x0c38e44cu
#define RH_CN_LABEL_CREATE 0x0c3ab22cu
#define RH_CN_LABEL_TEXT 0x0c3b38b0u
#define RH_CN_COLOR 0x0c3839aeu
#define RH_CN_TEXT_ALIGN 0x0c383a10u
#define RH_CN_FONT_STYLE 0x0c3839e4u
#define RH_CN_ALIGN 0x0c38361cu
#define RH_CN_FONT_RESOLVE 0x0c509354u
#define RH_CN_FONT_CREATE 0x0c495010u
#define RH_CN_FONT_DESTROY 0x0c492504u
#define RH_CN_WEEKDAY 0x0c542d5cu
#define RH_CN_SNAPSHOT 0x0c6a2a48u
#define RH_CN_SNAPSHOT_FREE 0x0c6a2aa4u
#define RH_CN_OBJECT_DELETE 0x0c384488u
#define RH_CN_GPU_FINISH 0x0c91e66cu
#define RH_CN_CACHE_SYNC 0x0c92e6d4u
#define RH_CN_CACHE_INVALIDATE 0x0c7211e8u
#define RH_CN_PREMULTIPLY 0x0c9079f8u
#define RH_CN_INVALIDATE_BUF 0x0c8b7800u
#define RH_CN_GENERATE 0x0c5478f0u
#define RH_CN_FONT_DAY 0x200c45dcu
#define RH_CN_FONT_WEEK 0x200c5af0u
#define RH_CN_RECEIPT_GENERATE 0x47f0e92du
#define RH_CN_RECEIPT_GENERATE_NEXT 0x4b66b0ccu
#define RH_CN_RECEIPT_CLOCK 0xb084b5b0u
#define RH_CN_RECEIPT_LOCALTIME 0x460db5b0u
#define RH_CN_RECEIPT_SNAPSHOT 0x4604b5f8u
#define RH_CN_RECEIPT_FONT_CREATE 0x4ff0e92du
#define RH_CN_RECEIPT_FONT_DESTROY 0x41f0e92du
#define RH_CN_RECEIPT_GPU_FINISH 0x4c17b510u
#define RH_CN_FONT_NAME_DAY 0x2cbc66d7u
#define RH_CN_FONT_NAME_WEEK 0x2cbc66c7u
#define RH_CN_FONT_FALLBACK 0x2ca12a38u

#endif
