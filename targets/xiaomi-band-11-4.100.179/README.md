# Xiaomi Band 11 4.100.179 exact identity recovery

Physical device: **NOT_PROBED**. The identity work below does not change any
framework symbol policy, approval state, or capability. Restricted/PENDING
states remain unchanged. The specific visibility record's static recovery is
documented below; other CANDIDATE records keep their existing states. GPU/cache fault recovery, pixels,
performance, and framework restart safety are not established by static recovery.

The input OTA is `../../mifw/miwear.watch.q66tc_v4.100.179_full_a93678a9.bin`, a ZIP
container. Its `vela_ap.bin` member is 12,293,060 bytes, with SHA256
`307adabe0ff61c77be73d045a8260e279cddd131c7a535a2678eb34199278a2b`.
The local extraction is `build/firmware-analysis/vela_ap_4.100.179.bin`; firmware
and the annotated IDA database are local build artifacts, not source deliverables.
XIP starts at `0x0c0c0000`; cached flash starts at `0x2c0c0000`.
Initialized DATA is copied from `0x2cc4919c` into
`[0x2007dcc8, 0x200add84)`.

`address-map.json` has schema 1. `addresses` maps **.177 baseline** entries and
data addresses to .179, including the extra native calendar emulation helpers.
Function translations use even entries; data addresses are preserved exactly,
including odd string addresses. `macros` records each header/QuickApp constant,
its .177 source value, an exact framework symbol when available, and AP byte
witnesses. A witness address is an XIP or cached-flash VA; subtract the matching
base above for its AP file offset. Witnesses include byte length, bytes, and
SHA256; receipt witnesses contain their exact four bytes as well.
`helpers` additionally records calendar harness translations and the generator
end. The `unresolved` list is empty for the implemented identity scope.

The module requires framework target pack **revision 2 or newer** for .179.
Revision 2 supplies `callable_address = 0x0c381d4d` on the existing
`background_visible_area` record and regenerates its veneer/Rust bindings.
The exact .177-to-.179 body/ABI review below supports this specific record's
`STATIC_RECOVERED` status and `proof.static = recovered`, recorded in
`EVID-RESOURCE-VISIBILITY-4179-001` with verdict `STATIC_RECOVERED`. Its policy
remains `restricted`, approval remains `PENDING`, and device proof remains
`not_probed`; no approval is granted. Other .179 candidates remain unchanged.
This respects the framework invariant that a `CANDIDATE` record has no callable.

The main source of reviewed function/data correspondences is the adjacent
framework pack `Canopus-Private/targets/xiaomi-band-11-4.100.179/symbols/`, together
with `evidence/fw-match/port-155-to-179.json`, `manual-overrides.json`, and
`EVID-PORT-4179-001.json`. The module's .177 address map links the original
constants to their .155 source identities. Calendar-specific records in the .179
pack are also checked directly against the stock .179 generator. Additional
background APIs are selected by individual .177 entry anchors, then checked
against complete .179 function bodies, native caller/constructor relationships,
and their recovered ABI in IDA. No global relocation delta is applied.

## Native roots, caches, and background owners

| Identity | .179 value | Exact AP evidence |
| --- | --- | --- |
| Default display slot | `0x200bd0a4` | Width `0x0c381630` and height `0x0c381650` accessors |
| LVGL global, image cache, header cache | `0x200bd08c`, `0x200bd1b4`, `0x200bd1b8` | LVGL initializer `0x0c3a94c4`; decoder/cache consumers |
| Image/cache classes | `0x2ca12e7c`, `0x2ca14a88`, `0x2ca14b08` | Image constructor `0x0c3ae118`; exact cache allocation classes in initializer |
| POSIX driver/cache/open slot | `0x200bd25c`, `0x200bd260`, `0x200bd268` | Initializer writes `/`, 4096, and callable `0x0c3a60c1` |
| Draw-buffer handlers | `0x200bd18c` | Initializer and decoder `0x0c39c078` pass this table to `0x0c386c14` |
| Launcher root/cache | `0x200c5f10`, `0x200c5f0c` | Main owner creation in `0x0c547e04` and update `0x0c547ac0` |
| More-page root/cache | `0x200c5adc`, `0x200c5ad8` | Owner creation `0x0c548a7c` and update `0x0c548804` |
| Launcher list class | `0x2ca436ec` | Constructor wrapper `0x0c542404`; class constructor is `0x0c5427c5` |
| Control-center presenter | `0x200c5668` | Creator `0x0c54a44c`; root is presenter +8; its child uses the base class |
| Notification container/list | `0x200c633c`, `0x200c6240` | Dispatcher `0x0c55c5cc`; list creator `0x0c557c1c` |
| Base/notification list class | `0x2ca3bbec`, `0x2ca3bbc8` | Constructor wrappers `0x0c6a350c`, `0x0c6a1de4` and class inheritance |

Object callback add/remove remain `0x0c37fbec` / `0x0c37fc32`. The rectangle
initializer at `0x0c38e03c` zeros 116 bytes; rectangle drawing at `0x0c38e47c`
consumes the background source at descriptor +48 and creates native draw tasks.
Display event-list add is `0x0c3a3ec4`; display callback removal is
`0x0c381e66`; layout update is `0x0c383f00`; visibility is `0x0c381d4c`.
Watchface snapshot acquire/release are `0x0c5f12ec` / `0x0c8f2acc`.

The visible-area identity has an explicit proof boundary: the framework's .155
comparison changed the body size from 222 to 218 bytes and originally left the
record **CANDIDATE**. The subsequent exact .177-to-.179 body/ABI review recovers
this specific record as **STATIC_RECOVERED**, under the evidence named above.
Comparing exact .177 to exact .179 yields the same
218-byte body and 83 instructions. Every mnemonic/operand matches except six
`BL` targets: `0x0c907ba4 -> 0x0c907c84` (10-byte null-checked +44 getter),
`0x0c3a38dc -> 0x0c3a38e0` (30-byte area-margin leaf), and
`0x0c3a3918 -> 0x0c3a391c` (70-byte intersection leaf), each called twice.
All three complete target leaf bodies are byte-identical to their .177 sources.
The JSON stores both full-body hashes, every changed call site, and all three
leaf hashes. This establishes the local address/ABI correspondence within the
inspected AP; it does not establish general semantic approval, object lifetime,
validity of arbitrary runtime inputs, or device acceptance. The height accessor
at `0x0c381650` is separately byte-identical across its complete 32-byte body.

GPU finish is `0x0c91e66c`, and D-cache synchronization is `0x0c92e6d4`.
The exact invalidation thunk is **`0x0c7211e8`**, with bytes
`5ff800f015582700`, jumping to ROM `0x00275814`. Its body matches the .177
`0x0c721248` thunk. The ROM implementation is outside the AP and remains
unverified; an address shift inferred from neighboring functions would select
a different thunk.

## Fonts and calendar

The .179 font metrics destructor is the reviewed empty callback at
**`0x0c39fcfc`** (callable `0x0c39fcfd`), immediately before metrics compare at
`0x0c39fd02`. The .177 header points to another identical empty body; the .179
framework's manual correction rejects that unrelated stub. The font face cache
uses a 28-byte node and the exact callbacks in the new header.

Two stale .177 header offsets are corrected independently in the new header:
font-manager add-path at `0x0c492458` reads the manager from `*uikit +28`, and
resolver `0x0c490e54` reads the path-list head at `manager +28`. Therefore both
`RH_FW_UIKIT_MANAGER_OFFSET` and `RH_FW_MANAGER_PATH_LIST_HEAD_OFFSET` are 28.
The existing runtime code already uses these +28 fields directly.

The stock generator occupies **`[0x0c5478f0, 0x0c547a8e)`**. Its native call tree
confirms root/image/label APIs, text properties, day/week style handles
`0x200c45dc` / `0x200c5af0`, and file output paths. Calendar publication at
`0x0c54781c` accesses launcher slot `0x200c5d88`. Startup style creation uses
font selector 1 / size 48 for the day and selector 0 / size 20 for the weekday.
The exact family strings are `MiSans-Medium` at `0x2cbc66d7` and
`MiSans-Demibold` at `0x2cbc66c7`; these are byte addresses, not Thumb pointers.
The fallback font identity is `0x2ca12a38`.

All eight calendar receipt words are read from the actual .179 AP, including
the word at generator +4. Their values happen to equal .177; they are not
accepted merely because the previous version used them. Calendar header code
identities remain even because the native call wrapper supplies the Thumb bit.
Pointer-comparison callbacks and directly callable background/FW constants
retain their required Thumb bit.

The additional calendar harness helpers map individually as follows:

| .177 baseline | .179 entry | Role |
| --- | --- | --- |
| `0x0c720f08` | `0x0c720f08` | Stock generator zero-fill call |
| `0x0c34af78` | `0x0c34af78` | Stock generator formatting call |
| `0x0c4f87fc` | `0x0c4f8804` | Image alignment helper |
| `0x0c384c40` | `0x0c384c40` | Pixel helper |
| `0x0c383434` | `0x0c383434` | Local style setter |
| `0x0c547a0e` | `0x0c547a8e` | Generator end, exclusive |

## QuickApp package lookup

The exact service pointer slot is `0x20084e74`; its initialized DATA word comes
from cached flash `0x2cc50348` and equals **`0x2ca3b5cc`**. The vtable's slot 3
(+12 bytes) is **`0x0c69faeb`**, matching the exact `app_lookup_package` symbol.
Its wrapper at `0x0c69faea` calls `0x0c6aab08`, whose native hashmap lookup reads
**`0x20084e6c`**. This is the package registry, independently checked rather
than choosing the neighboring registry.

Native install `0x0c6a9798` duplicates package and icon pointers at app-record
+12 and +16, then registers the +12 package key into `0x20084e6c`. The module's
.179 branch retains these offsets and the `/data/quickapp/app/` path contract.
The existing `0x0cd00000` readability bound is conservative and contains the
exact .179 AP XIP interval; it is not an inferred native symbol address.

The per-macro witness set was checked against the extracted AP and the selected
framework records. Module build, host tests and selected native emulation are
reported by the parent integration task separately; no static identity result
here establishes physical-device acceptance.
