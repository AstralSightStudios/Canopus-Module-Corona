# Xiaomi Band 10 Pro (p67) 3.101.043 UI Reload Audit

This document records the exact reverse engineering and audit evidence for porting
Resource-Hook to Xiaomi Band 10 Pro firmware `3.101.043` (`CONBINE_LTALM078_T3.101.043_08041658`).

## Exact 3.101.043 Firmware Evidence

- AP SHA256: `519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec`
- Extracted binary: `vela_ap.bin` (13,795,728 bytes)
- Architecture: ARMv8-M Mainline (Cortex-M33), Thumb-2, Little-endian, soft float
- Loader: `best1503_vela.py` maps XIP code at `0x0c0c0000`, cached flash at `0x2c0c0000`, and NC flash at `0x280c0000`. PSRAM execution alias is `0x1c000000` (backed by data alias `0x3c000000`).
- Target Selector: `defined(RH_TARGET_1043) && RH_TARGET_1043`

## Verified Layout and Entry Points

| Symbol / Layout | Address / Value | Evidence & Disassembly Witness |
|---|---|---|
| POSIX driver instance | `0x20103340` | `0xc1651b2`: `LDR R4, =0x20103340`. `STRB R0, [R4, #0]` sets `'/'`, `STR R6, [R4, #4]` sets cache size 4096. |
| POSIX cache size | `0x20103344` | Verified `4096u` (`0x1000`) at `[driver + 4]`. |
| POSIX open callback slot | `0x2010334c` | `0xc1651d6`: Stores open callback at `[driver + 12]`. |
| POSIX open callback | `0x1c057c51` | `0xc1651c4`: Literal `0x1c057c51` loaded into R5 and stored into driver open slot. Executes in PSRAM code view. |
| LVGL fs open | `0x0c169624` | `lv_fs_open` in `lv_fs.c`. Checks drive letter, invokes `driver->open_cb(drv, path, mode)`. |
| LVGL image object class | `0x2cce61ec` | Loaded at `0xc179f0a` (`lv_image_create`). Points to `lv_image_class` with base class `0x2cce0308`. |
| LVGL image setter | `0x0c17a2f5` | `lv_image_set_src` (entry `0x0c17a2f4`). Checks info query before invalidation and dimension update. |
| Image decoder get-info | `0x0c143799` | `lv_image_decoder_get_info` (entry `0x0c143798`). Called at `0xc17a314` by `lv_image_set_src`. |
| Decoded image cache slot | `0x2010329c` | `0xc166aa6`: Stores result of `lv_cache_create` (`"IMAGE"`) into `0x2010329c`. |
| Header cache slot | `0x201032a0` | `0xc166c12`: Stores result of `lv_cache_create` (`"IMAGE_HEADER"`) into `0x201032a0`. |
| Decoded image cache class | `0x2cce56f4` | Vtable passed to `lv_cache_create` at `0xc166aa0`. Node size set to 4 at `cache + 48`. |
| Header cache class | `0x2cce571c` | Vtable passed to `lv_cache_create` at `0xc166c0c`. Node size set to 4 at `cache + 48`. |
| Cache drop | `0x0c1667dd` | `lv_cache_drop` (entry `0x0c1667dc`). Unlinks entry, decrements/frees matching key payload. |
| Cache release | `0x0c1666ed` | `lv_cache_release` (entry `0x0c1666ec`). Decrements reference count and calls `free_cb` on zero. |
| Object screen tree walk | `0x0c13c691` | `lv_obj_tree_walk` (entry `0x0c13c690`). Traverses display screens (`disp + 692`, count `+ 720`) when root is null. |
| Object style getter | `0x0c1068a9` | `lv_obj_get_style_prop` (entry `0x0c1068a8`). Resolves style property merging object state at `+48`. |
| Object style refresh | `0x0c1070ad` | `lv_obj_refresh_style` (entry `0x0c1070ac`). Called by `lv_obj_set_local_style_prop` (`0xc107790`) at `0xc107892`. |
| Default display slot | `0x2010318c` | `lv_display_get_default` (`0xc13cca8`) returns `*(0x20103174 + 0x18) = *(0x2010318c)`. |
| Display invalidation | `0x0c105165` | `_lv_inv_area` (entry `0x0c105164`). Checks `disp->rendering_in_progress` and invalidates dirty rectangle. |
| Display horizontal resolution | `0x0c13cd55` | `lv_display_get_horizontal_resolution` (entry `0x0c13cd54`). Reads `[disp + 0]`. |
| Display vertical resolution | `0x0c13cd75` | `lv_display_get_vertical_resolution` (entry `0x0c13cd74`). Reads `[disp + 4]`. |
| Timer create | `0x0c587ed1` | `lv_timer_create` veneer at `0xc587ed0`. Confirmed in target symbols. |
| Timer delete | `0x0c588129` | `lv_timer_del` veneer at `0xc588128`. Confirmed in target symbols. |
| UIKit slot | `0x20103174` | Referenced by UIKit font manager wrappers at `0xc860382`. Manager pointer at `uikit + 28`. |
| Font manager add path | `0x0c86037d` | `uikit_font_manager_add_path` (entry `0x0c86037c`). Resolves manager and calls `0xc85f524`. |
| Font manager remove path | `0x0c860399` | `uikit_font_manager_remove_path` (entry `0x0c860398`). Resolves manager and calls `0xc85f63c`. |
| POSIX open / read / write / close | `0x0c1d0a29`, `0x0c1d129d`, `0x0c1d2641`, `0x0c1b9d81` | Standard NuttX VFS primitives confirmed in Canopus target pack. |
| Heap malloc / free | `0x0c1f903d`, `0x0c1f8ff9` | `sub_C1F903C` and `sub_C1F8FF8` confirmed as generic safe allocator pair (returns ENOMEM on failure without panic). |
