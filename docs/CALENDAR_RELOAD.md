# Native dynamic calendar regeneration on reload

Exact targets: Xiaomi Band 11 `4.100.139` / `4.100.155`, and Band 10 Pro
`3.101.043`. This is a narrowly scoped native calendar adapter, not a general
snapshot/canvas reload facility. The user reports successful device validation of
calendar reload on all three exact targets using the signed installers below.
This is **USER_REPORTED_PASS** for the visible calendar-reload workflow, not
instrumented GPU, storage-failure, allocation-pressure or restart-recovery acceptance.

## Trigger and ordering

A reload regenerates the calendar when either the old or current mapping affects
its compositing background, final/fallback icon, initial launcher calendar icon,
or `/resource/font/MiSans-Regular-All.ttf`. Directory rules and `@system`
exceptions retain the existing longest-match behavior. Unrelated themes do not
allocate a calendar snapshot or perform its native file write.

An explicit **new reload revision** also regenerates the calendar when mappings
are byte-equivalent: assets can have changed at the same paths. Repeating an
already-consumed revision does not regenerate. Removing a mapping checks the old
view so the stock background is regenerated too.

The coalesced transaction runs:

1. Retire mapped file-image cache keys.
2. Complete the existing font stage.
3. In a serialized UI timer, retire the exact calendar background key and invoke
   the target's native generation/publication chain once.
4. Refresh supported image/style owners.
5. Request asynchronous full-screen invalidation.

`activate()` never invokes the native calendar generator/notify. Rendering-busy,
missing display and invalid cache state defer the stage to a UI timer. Owner or
redraw retries do not repeat a completed generation. Cache retirement preserves
native reference ownership and never calls global `drop_all`.

On an identical-map calendar revision, unrelated image-cache keys are not retired;
the normal image-owner walk still runs afterwards. Its allocation/ownership limits
can keep the transaction pending, but do not cause repeated calendar generation.

## Band 11: regenerate a file, then publish it

```text
/resource/app/perpetual_calendar/launcher_icon.bin
                 + localized weekday/current day
                             |
                    native 112 x 112 snapshot
                             |
/data/app/perpetual_calendar/calendar_icon.bin
                             |
                  existing launcher image owner
```

Both APs have generator `0x0c5487a4` and publisher `0x0c5486d0`; individual
snapshot/free leaves and flash literals are verified per target rather than
assuming an address delta. Native generation owns its temporary screen, labels
and snapshot. The native writer retires the output pathname and writes its LVGL
header/pixels. Publication finds app ID 69 and applies the output path using the
native image setter, with native hidden/object-validity checks.

If the launcher app-list service has not initialized, this adapter skips the
request; normal native initialization generates the icon later with the resource
hook already resident. Hidden/unavailable widgets need not immediately adopt the
file. The adapter does not force a page reconstruction.

Directly mapping the `/data/.../calendar_icon.bin` output still replaces the whole
icon with the supplied image, including the date/weekday pixels. Native generation
writes the normal system output, while LVGL reads remain redirected. Background
mapping instead preserves the system's dynamic text.

## Band 10 Pro: notify the real registered app

The compositing background is
`/resource/app/perpetual_calendar/calendar_background_icon.bin`. Normal output is
a reused RAM snapshot; `/resource/app/perpetual_calendar/launcher.bin` is its
allocation/render-failure fallback.

The adapter verifies the exact runtime vtable at `*0x200eb658`, queries native app
ID 69 in the two native registration lists, and validates the real RAM record's
ID, bounded appid string and exact signal callback. No fake stack descriptor,
ROM descriptor mutation, retained app pointer or manual snapshot free/reset is
used.

Native notify `0x0ca6a004("com.xiaomi.miwear.perpetual_calendar")` performs the
registered name lookup, invokes signal 6, then synchronously dispatches launcher
event `0x2b` with the real app. The native calendar callback updates app+12 and the
native launcher consumer adopts that source. An absent/unrecognized app/vtable
is skipped rather than calling an unknown native interface.

## Evidence and limitations

- Per-target entry addresses and bounded AP byte-window hashes are in
  `targets/<exact-target>/calendar-reload.md`; matching restricted symbol/evidence
  records are in the neighboring Canopus target packs. No address-range expansion,
  verifier bypass or approval promotion is used.
- `tests/test_module.c --calendar` exercises UI ownership, busy retries, stage
  coalescing, unchanged-map revisions, removed mappings and unrelated themes.
- `tests/test_calendar_adapter.py` executes the source-compiled ARM adapter for
  all three address selections with native lookup/generation/retirement leaves
  modeled, including wrong owner/vtable/cache guards.
- `tests/firmware_reload.py` additionally executes the Band 11 adapter's exact-key
  retirement against each AP's native cache-drop instructions; unlink/heap and
  native calendar generation leaves are modeled.
- `tests/firmware_calendar.py` executes the actual generation/publication or
  notify/lookup/dispatcher/calendar callback and native string-hash lookup
  instructions. GUI, clock, formatting, heap, string comparison, snapshot and
  writer leaves plus registry storage are modeled as documented there.

Native generation has **no reliable success return ABI**. In particular, Band 11
writes the system file nontransactionally and ignores its writer return; storage
or allocation failures can leave an incomplete/missing output. A completed stage
means native work was requested, not verified pixels, successful file writing or
resource adoption. Status ABI is RHQ1 v6 in all current builds; no
calendar-success counter is invented.

Default font transactions now replace audited retained font wrappers before
calendar generation. A completed calendar stage does not independently certify
font or pixel adoption; the [font contract](FONT_RELOAD.md) retains its healthy-UI,
immutable-generation and unsupported-recovery restrictions. The
rendering flag is not a general GPU-idle proof. The module adds no forced GPU
wait/reset, global animation teardown or full UI restart.

## User-reported device validation

After receiving the signed installer packages, the user reported that the latest
10 Pro package worked normally, then explicitly confirmed successful validation
of **both** Band 11 firmware versions. No device logs, screenshots, iteration
counts or fault-injection results were supplied; the report is not expanded into
broader lifecycle or GPU guarantees.

| Exact target | Signed ELF SHA-256 | Installer ZIP SHA-256 | Result |
| --- | --- | --- | --- |
| Xiaomi Band 10 Pro 3.101.043 | `ebcba54e85172ddbf87686c7e3ad63a446ab7c4b9fceffee5651ddfb8e1c1be7` | `2580f01149c017165d0099569f66bc4439fca565cfedeef9cba63f5763de3248` | USER_REPORTED_PASS |
| Xiaomi Band 11 4.100.139 | `0a65773a84f0b5797de8061c29ba05aea0a9d2c892bd661791daa17a706e875d` | `73ad1fe4c8a06cf3409478662bca464239a5d3bb396f0495c42404574bc5eb8b` | USER_REPORTED_PASS |
| Xiaomi Band 11 4.100.155 | `f78587f7827f7517b621e8de9f8a9f7212c14c461290d1b533250c422cc3c340` | `73ad1fe4c8a06cf3409478662bca464239a5d3bb396f0495c42404574bc5eb8b` | USER_REPORTED_PASS |

Installer names: `resource-hook-calendar-band10-pro-1043.zip` and
`resource-hook-calendar-band11-139-155.zip`. Their ZIPs and build artifacts remain
ignored generated files, not source-controlled releases; no signing key is committed.
Exact-target symbol records remain restricted/STATIC_RECOVERED/PENDING; this
user report does not promote API approval or remove the documented limitations.

## Reproducible checks

```sh
CC=/usr/bin/clang sh scripts/test-host.sh
CLANG=/usr/bin/clang build/firmware-tests/bin/python tests/test_calendar_adapter.py

for v in 139 155; do
    RESOURCE_HOOK_TARGET=xiaomi-band-11-4.100.$v \
    RESOURCE_HOOK_FIRMWARE=build/firmware-analysis/vela_ap_4.100.$v.bin \
    CLANG=/usr/bin/clang build/firmware-tests/bin/python tests/firmware_reload.py
    build/firmware-tests/bin/python tests/firmware_calendar.py \
        --target "$v" --firmware build/firmware-analysis/vela_ap_4.100.$v.bin
done
build/firmware-tests/bin/python tests/firmware_calendar.py \
    --target 1043 --firmware build/firmware-analysis/vela_ap_3.101.043.bin
```

Each target still requires its own strict ELF verification/signing receipt;
new builds and fault scenarios require recoverable hardware validation. Source
builds produce an **unsigned ELF**, not an installed module or a signed delivery
package. The user-reported validation above applies to the identified signed
artifacts, not automatically to future builds.
