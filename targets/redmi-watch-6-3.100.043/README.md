# REDMI Watch 6 3.100.043 (P65)

This is an exact AP port. It does not cover Band 10 Pro `3.101.036`, other
REDMI Watch firmware, or other LVGL v9 builds.

- Input OTA: `miwear.watch.p65_v3.100.043_full_662aa7db.bin`.
- AP SHA-256: `a19b601477569765103eaf468b937bd8026b4d68ffa5c896bde222a79e475858`.
- Input loader: `../../mifw/best1503_vela.py`.
- Target flag: `RH_TARGET_P65=1`.
- Physical device: **NOT_PROBED**. Static recovery and instruction tests are
  separate from a firmware boot, GPU verification, or production approval.

## Core adapter

`core-evidence.json` records exact function bodies, startup copy ranges, RAM
slots, class identities, and private structure offsets. The P65 adapter uses
its own POSIX driver and callback, selective image/header cache retirement,
image owner refresh, and full-screen invalidation request.

P65 differs from the existing ports in more than function addresses:

| Contract | P65 |
|---|---|
| POSIX driver / open slot | `0x3c4501d0` / `0x3c4501dc` |
| POSIX open | `0x0c7357d5` (Thumb) |
| Initial POSIX cache size | `0` |
| Cache list payload size / head offsets | `44` / `48` |
| Cache drop / release | Three arguments; `user_data = 0` |
| Image source type offset | `92` |
| Active display screen offset | `688` |

Referenced cache payloads stay alive after retirement until native release.
Only affected FILE keys are retired; unrelated cache entries are retained.
The original POSIX callback receives slash-free LVGL paths, prepends `/`, and
converts native file descriptors to `fd + 1`.

The framework's previous candidate `lv_cache_drop` at `0x1c1501d4` is socket
code; its candidate `lv_cache_release` at `0x00387db8` allocates object attributes.
Neither is used by this module. Do not promote or call those candidates.

## Default full feature set

All P65 features are compiled into the ordinary build without extra feature
environment variables: image redirection/reload, QuickApp package icons,
transactional system-font replacement, custom calendar text/images, four native
background surfaces, and module-owned snapshot blur. System/default
configuration retains the stock appearance.

- `font-reload-audit.md/json` covers the P65 context, count-cache, draw-queue
  and wrapper ownership contracts; P65 has no P67 vector-label extension.
- `calendar-audit.md` covers the native 96x96 icon and module-owned immutable
  ARGB8888 file banks. Restoration drains retained render work and retires
  the logical icon cache keys before clearing redirection; the original stock
  icon file is never overwritten.
- `background-audit.md` covers launcher, control center, notifications and
  widgets, screen-fixed drawing and native snapshot/live foreground lifecycle.
- `blur-evidence.md/json` covers the owned BGR888 capture, downsampled RGB565
  output, exact cache hooks, raw GPU finish and deferred reclamation.

Native Manager text font ownership is separate from transactional replacement
of system fonts. The companion Canopus changes (P65 pack revision **3**) provide
the required exact symbols, P65-specific display/draw-unit offsets, native
allocator and **DELETE event35**, plus `432×514` display/content geometry.
Rebuild the matching Supervisor. `features-audit.md` documents its native font
and QuickApp registry identities. Static and modeled native-leaf tests do not
establish hardware rendering, GPU fault recovery or framework restart safety.

## Build and verification

```sh
sh scripts/build.sh redmi-watch-6-3.100.043
python3 tests/test_p65_capabilities.py
python3 tests/test_p65_font.py
python3 tests/test_p65_backgrounds.py
python3 tests/test_p65_blur.py
build/p65-tests/bin/python tests/firmware_p65.py
build/p65-tests/bin/python tests/firmware_p65_features.py
build/p65-tests/bin/python tests/firmware_p65_font.py
build/p65-tests/bin/python tests/test_p65_calendar_native.py
FIRMWARE_PYTHON=build/p65-tests/bin/python \
  python3 scripts/build-delivery.py dist/p65-3.100.043-full \
  --target redmi-watch-6-3.100.043 --firmware-tests
```

The instruction suite requires Python `unicorn` and `pyelftools`, Clang/LLD,
and the exact OTA or `RH_AP_P65=/path/to/vela_ap.bin`. It verifies fingerprints,
executes the native POSIX callback, display accessors and cache drop/release,
and executes the compiled adapter against P65-shaped caches and display data.
The feature suite executes the native font reference drain and QuickApp lookup.
Native OS, mutex and selected callback leaves are modeled. These checks do not
claim a successful firmware boot, hardware graphics behavior, or real-device
acceptance. Firmware binaries and IDA databases are not source deliverables.
