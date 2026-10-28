# P65 3.100.043 feature identities

Exact AP SHA256: `a19b601477569765103eaf468b937bd8026b4d68ffa5c896bde222a79e475858`.
Source: supplied `miwear.watch.p65_v3.100.043_full_662aa7db.bin`, ZIP member
`vela_ap.bin`; extracted AP and IDA database remain outside the repository in
`../../mifw/analysis-p65-043/`. Function extents and full-body hashes are in
`features-audit.json`. All findings are static/instruction-emulator evidence;
physical device and GPU rendering remain **NOT_PROBED**.

## Native Manager font lifecycle

Framework P65 records, `loader/native-bindings.json`, generated private header
and Rust generated records now carry the reviewed identities below. Policies
remain `restricted`, approval remains `PENDING`; no public export was added.

| Identity | Exact callable/global |
|---|---|
| `void *native_font_create(void *manager, const void *key8)` | `0x0c795db9` |
| `void native_font_release(void *manager, void *font)` | `0x0c795fe5` |
| `void native_font_wait(void *vg_unit)` | `0x0c72ef2d` |
| UIKit root slot, font manager at root+28 | `0x3c450210` |
| draw-unit list slot | `0x3c450134` |
| display-list header | `0x3c450034` |
| native VG dispatcher identity | `0x0c7287f9` |
| native SW dispatcher identity | `0x0c7222bd` |

The key is `{family pointer, uint16_t pixels, uint16_t style}`. The factory
returns an owned 40-byte managed font wrapper with backing record at +36.
Labels borrow the wrapper. The host must retain it until label references and
queued CPU glyph references have retired, then call the matching release with
the original manager. Native internal allocation failure handling is not
comprehensively safe.

The former candidate wait `0x1c12e7bc` is a false match: its AP source
`0x0c21e580` implements a two-argument style initialization helper. It was
replaced with the actual native wait `0x0c72ef2c`, which calls lower GPU finish
`0x0c8bc080`, drains pending containers at unit+36/+44 and glyph unit+28 through
`0x0c72d83c`, then clears submission counter+52. It drains even after GPU errors;
CPU reference retirement does not certify successful GPU rendering or recovery.

P65 differs from existing P67/Band11 structures. The framework generator emits
these P65-only offset macros; `canopus_ui_font.inc` must honor them:

| Macro | P65 | Prior layout |
|---|---:|---:|
| `FW_UI_FONT_DISPLAY_NEXT_OFFSET` | 788 | 796 |
| `FW_UI_FONT_DISPLAY_LAYER_OFFSET` | 672 | 680 |
| `FW_UI_FONT_UNIT_DISPATCH_OFFSET` | 12 | 16 |
| `FW_UI_FONT_UNIT_ACTIVE_OFFSET` | 24 | 32 |

Display payload is784, list head+4, rendering byte58 bit2; layer task-head+100
and next+108 remain unchanged. Display screens+684, active screen+688,
screen-count+712 also differ from the former layout. Native constructor AP
`0x0c0cec60` runs through SRAM callable `0x00374c49` after startup copy.

Framework evidence: `EVID-UI-P65-FONT-LIFECYCLE-001.json`. Regeneration used a
freshly rebuilt `canopus-cli`; rebuilding CLI and target generation passed.

## Quickapp icon lookup

`include/resource_hook_p65_features.h` supplies the exact `RH_P65_QA_*` values:

| Identity | Value |
|---|---|
| Service slot / initialized table | `0x3c3cf934` / `0x3c350b30` |
| Active package registry slot | `0x3c3cf92c` |
| Package lookup (table+12) | `0x1c15165d` |
| Record package / icon pointer offsets | 8 / 12 |
| Canonical icon root | `/data/quickapp/app/` |
| AP XIP end (exclusive) | `0x0cbff484` |

PSRAM lookup corresponds to AP `0x0c241420`, reads the active registry and
calls hash lookup `0x0ca3bc5c` via a native thunk. Its djb2 hash, power-of-two
bucket layout, node key+4/length+8/value+12/next+16 match the module preflight.
The native app getter `0x0c240e7c` selects package+8 and icon+12. Table slot3
is package lookup; slot1 is numeric-ID lookup and cannot be substituted.
The table is initialized by AP word `0x0c4bf6f8`. Path constructor
`0x0c82e634` uses `/data/quickapp/app/%s/%s` (runtime string `0x3c25927c`);
P67's `/data/app/` prefix is wrong for this device.

The exact adapter is enabled by default. Native registry execution and the
compiled adapter's pointer/ownership boundaries are tested. Device icon redraw
remains NOT_PROBED.

## Default capabilities and validation

System-font hot reload, native calendar, four background surfaces and blur are
now implemented and enabled in the standard P65 build. Their independent exact
contracts are recorded in `font-reload-audit.md`, `calendar-audit.md`,
`background-audit.md` and the blur evidence. Manager label fonts remain separate
from system-font transactions. P65 DELETE is event35; the generator and native
font owner use that exact event when retiring label references.

`uv run --with unicorn python3 tests/firmware_p65_features.py` passed two tests:

- Native wait/drain executes original code, calls nine controlled release
  callbacks across all three pending containers, resets both arrays' counts
  and submission counter on GPU success and error. GPU finish/logging and
  release callbacks are controlled leaves; no GPU emulation claim.
- Native PSRAM package lookup executes original hash and bucket traversal,
  returns the registered record and returns0 for a missing package. `strcmp`
  is a controlled libc leaf; this does not claim full libc execution.

Mirage's explicit P65 profile documents432×514 at
`Packages/MirageKit/Sources/MirageKit/Models/DeviceProfile.swift:409`.
The exact AP constructor forwards runtime dimensions; a separate generic
800×480 helper exists and must not be mistaken for panel evidence. Core
resolution accessors and a booted display provide the runtime confirmation.
