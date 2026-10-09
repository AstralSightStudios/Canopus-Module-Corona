# Configurable transparent calendar (Scheme B)

## Current scope

The replacement renderer is recovered for **Xiaomi Band 11 4.100.177, .139 and .155**.
The .139/.155 backports have static/model validation only (NOT_PROBED); see
their target `background-calendar.md` notes. Existing .177 evidence:
AP SHA256 `ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b`.
It has host/Unicorn coverage and **USER_REPORTED_PASS** for the visible calendar
workflow: on 2026-10-07 the user reported successful device validation. This is
not instrumented GPU/fault acceptance or a byte-exact pixel comparison.
All four supported exact targets provide native date/render adapters; there is
no runtime supported/unsupported probe. Instruction-receipt mismatches fail the
date/render operation (`-2155`/`-2156`) and preserve the previous publication.
Band 11 reload does not invoke the old native GENERATE or NOTIFY chains.
Band 10 Pro 3.101.043 uses its separate RAM-source adapter for custom calendar
configuration and retains the stock calendar for defaults. Its clock, snapshot,
font and application-publication ABI is not a relocated Band 11 adapter; the
existing file-bank writes remain in place pending separate consumer validation.

Both renderers share item validation/context selection, but retain native font
and object creation. Configured image cache keys are deduplicated by path
contents, including repeated images across month/day/weekday. Retirement still
runs before rendering and again at publication to evict reentrantly cached old
output; this optimization does not merge those two lifecycle boundaries.

The user has explicitly approved the **15 newly recorded exact private native
entries** in the adjacent Canopus `.177` pack (revision 2). Each has a recovered
prototype, ownership/context constraints, exact AP function-byte receipt and
human promotion record. Policy remains `restricted`, status `STATIC_RECOVERED`,
device proof `not_probed`; no public SDK wrappers, address-range expansion or
verifier exception is added. Approval removes the previous exact-symbol blocker,
not the separate signing/install or physical-device acceptance requirements.

## Rendering and redirection

```text
Existing mappings -> /resource/app/perpetual_calendar/launcher_icon.bin
                                   + month/day/weekday
                                            |
                       module-owned 112 x 112 ARGB8888 snapshot
                                            |
                     inactive calendar-alpha-{0,1}.bin bank
                                            |
                  checked header/pixel writes, publish ready bank
                                            |
Launcher opens /data/app/perpetual_calendar/calendar_icon.bin
                       -> existing POSIX read hook -> ready bank
```

The files are inside the target's app-private `RH_APP_FILES_ROOT`. No change is
made to firmware Flash or the system-generated file. The native system may
continue rewriting its opaque file; reads select the independent completed bank.
The read hook does no rendering, configuration/clock I/O, allocation, or GPU work.
An explicit mapping for the **final** system icon still wins and replaces the
whole icon. `@system` removes theme mapping, not the module's default Alpha
renderer. On an initial generation/open failure, ordinary original-resource
fallback remains available; permanent transparency is not promised under faults.

A serialized UI timer owns configuration, rendering, publication and retries.
Normal refresh order is mapped-cache retirement, font transaction, calendar,
image owners, display invalidation. Startup, a new configuration revision, date
changes (including year/month), time-zone changes that change the date, and a
changed localized weekday string request generation. The one-second watcher
checks native local time outside rendering; unchanged dates do not render.
Refresh completion means checked writes and a publication request, not observed
pixels or persistent-storage durability. The platform's void close cannot expose
close errors. Hidden/missing Launcher owners adopt the logical key on later
native display; no forced page rebuild is used.

Only relevant background, final-output, physical-bank and configured-image cache
keys are retired, before rendering and again at the publication boundary to
remove any old-bank decoder repopulated by reentrant callbacks. Referenced native cache entries keep native ownership. There
is no global drop-all. Failed snapshot/GPU completion retains one bounded native
tree/fonts/buffer until a later successful fence; restart/unload during pending
GPU work and fault recovery remain unsupported.

## Defaults: native attributes, not an unverified pixel guarantee

Without `calendar`, with `calendar: {}`, or with no `calendar.cfg`:

| Item | Native default in .177 |
| --- | --- |
| month | none |
| day | local day, no leading zero; native MiSans-Medium 48 wrapper; black; top-mid `(0,34)` |
| weekday | native localized string; native MiSans-Demibold 20 wrapper; packed color `0x3482ff`; top-mid `(0,17)` |

The original image object and label attributes are replicated, including native
font resolution and language formatting. The default renderer snapshots the
**image child**, not its opaque parent. Keeping the native parent style does not
flatten that parent into the image-child snapshot. Background files continue
through ordinary `mappings`; there is deliberately **no calendar.background**.

ARGB8888 (native format 16), unlike original RGB565 (18), preserves Alpha.
RGB565 quantization and blending onto black differ from ARGB8888 rendering;
antialiasing/translucent edge colors need not be byte-identical. Host tests
compare original .177 instructions' object attributes against the replacement,
not physical raster output. The earlier proposed `#F0403F` weekday default and
unconditional pixel-identity guarantee were incorrect.

## Resource pack declaration

Add `calendar` to the existing **corona.json** root (legacy canora.json remains
supported); there is no separate calendar background field or new pack format.
See `examples/calendar/corona.json` for a valid text-only manifest.

```json
{
  "calendar": {
    "month": {
      "type": "text",
      "x": 0, "y": 2, "align": "top_mid",
      "size": 14, "color": "#FFFFFF",
      "context": ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
    },
    "day": { "type": "text", "size": 48 },
    "weekday": { "type": "none" }
  }
}
```

- Three optional items: `month`, `day`, `weekday`. Omitted items use defaults.
- `type`: `text`, `image`, `none`. `none` allows only `type`.
- Placement: optional `x/y`, integer offsets `-112..112`, relative to optional
  `align` (`top_left`, `top_mid`, `top_right`, `center`); defaults above.
  Drawing is clipped to the fixed 112x112 canvas; coordinates are not absolute
  centers unless `align` is center.
- Text: optional `size` (`0` = native wrapper, or `8..72` pixels), optional
  `color` (`#RRGGBB`, omitted = native), optional `context` (empty/omitted =
  numeric month/day or system-localized weekday). Nonempty arrays have exactly
  12/31/7 entries. Empty text entries intentionally hide that date's text.
  Each text is at most 63 UTF-8 bytes, no controls or invalid Unicode.
- Images: exact 12/31/7 `context` paths to package-relative **BIN files**, all
  validated against the inventory. Each selected image keeps its BIN dimensions;
  resize is not implied by placement. Image color or nonzero size is rejected.
- Indexing: month January first, day 1 first, **weekday Monday first**.
- Layer order: background, month, day, weekday.
- Priority: highest active pack with a `calendar` declaration wins as a whole;
  items are not merged between packs. An explicit `{}` masks lower declarations
  and resets to native defaults. Background priority stays ordinary mappings.
- Manager retains all referenced image-pack directories across publication
  failures and acknowledged-generation cleanup. Optional known runtime identity
  rejects unsupported declarations before publication; C independently rejects
  non-default unsupported CFGs.

## Transport and failure handling

Manager emits `internal://files/calendar.cfg` before signaling reload:

```text
version=1
<item> TAB <type> TAB <align> TAB <x> TAB <y> TAB <size> TAB <color> TAB <count> LF
<context 0> LF
...
```

Items are unique; color is `native` or six uppercase hex digits, without `#`.
Image paths are expanded to `themes/<themeId>/<package path>` in the CFG; C
prepends the target-specific app root. Missing declarations produce exactly
`version=1\n`; removing configuration therefore restores defaults, not stale
custom settings. Bounded native parsing validates UTF-8, counts, coordinates,
colors and paths independently. Malformed/read/allocation failures preserve the
active config and ready bank. Config and mapping publication are reserved
against nested activation; rendering never runs while the read hook owns data.

| Error | Meaning |
| --- | --- |
| -2151 | configuration allocation |
| -2152 | configuration open/read |
| -2153 | invalid/oversized configuration |
| -2154 | UI/display/cache busy or unsafe to render |
| -2155 | native date/firmware receipt invalid |
| -2156 | image/font/snapshot/GPU/header/write failure |

Legacy artifacts may report `-2157` for unsupported calendar overrides; current
four-target builds do not emit that capability error.

## User-reported device validation

On 2026-10-07, after the .177 implementation and exact API approval, the user
reported successful device validation and requested commits. Result:
**USER_REPORTED_PASS** for the current transparent/configurable calendar workflow.
No installed-artifact hash, logs, screenshots, configuration matrix, iteration
counts or fault-injection results were provided. The report is not expanded into
per-API DEVICE_PROVEN status, exact raster identity, allocation/storage/GPU fault
handling, restart safety, or other-target acceptance. No signed installer was
produced by the assistant in this session; local build verification is separate
from the user's device result.

## Reproducible checks

```sh
CC=/usr/bin/clang sh scripts/test-host.sh
node tests/test_manager_calendar.cjs
python3 tests/test_calendar_native.py
python3 tests/test_calendar_adapter.py
python3 tests/test_calendar_allowlist.py
CC=/usr/bin/clang sh scripts/build.sh xiaomi-band-11-4.100.177
```

Native Python tests require `unicorn`, `capstone`, `pyelftools`, and the exact
fingerprinted AP; set `RH_CALENDAR_FIRMWARE=/path/to/vela_ap.bin`. Native leaves
model clock, LVGL objects, font allocation, files and GPU completion, not pixels.
`tests/firmware_calendar.py` remains historical evidence for the old system
GENERATOR/NOTIFY ABIs, not evidence that the module invokes them today.

At implementation time portable host tests, calendar Manager tests, and ten
native tests passed. Four additional allowlist regressions check schemas, exact
AP byte receipts, private-only generated bindings and removal/wrong-target
negative controls. The combined Manager runner has an unrelated existing
`test_manager_layout.cjs:76` failure (`undefined` vs `60px`). Exact-symbol
approval is not a signature, installation or device result. The `.177` unsigned
module now passes strict ELF verification (20 sections, zero undefined symbols).
Scoped Canopus generation/SDK checks pass; its full workspace still has unrelated
`.036` generated-artifact staleness around `poll_notify`. User-reported device
success above does not establish a comprehensive acceptance matrix for
transparent corners, byte-exact text/layout, every custom image/font, cross-midnight,
language changes, same-path revisions or recoverable I/O/allocation failures.
