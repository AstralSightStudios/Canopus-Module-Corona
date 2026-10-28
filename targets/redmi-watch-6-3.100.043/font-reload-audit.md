# P65 exact system-font reload transaction

AP SHA-256: `a19b601477569765103eaf468b937bd8026b4d68ffa5c896bde222a79e475858`.
`font-reload-audit.json` pins every required callable to the exact AP bytes;
framework evidence is `EVID-FONT-P65-TRANSACTION-001`. Policies stay
`restricted / PENDING / not_probed`; static and modeled execution evidence do
not imply device approval.

The default P65 build uses the production checked font transaction. It prepares
new faces and child caches, verifies source files and native ownership, then
publishes registry paths, backing records and wrapper payloads together. A rule
removal restores the fingerprint-verified stock face. A failed preparation keeps
the previous paths, owners and referenced cache payloads. Immutable non-stock
font generation paths are mandatory, as on the existing targets.

## Exact contracts

| Identity | P65 |
|---|---|
| Font callbacks (metrics/outline/release) | `0x0c734999` / `0x0c734e49` / `0x0c734e2d` |
| Native descriptor factory | `0x0c7344fd` |
| UIKit / FreeType context slots | `0x3c450210` / `0x3c450204` |
| Context structure | library0, FTC manager4, cmap8, intern list12, outline event24, face cache28 |
| Face / metrics / outline cache payload | 24 / 32 / 8 bytes |
| Metrics / outline count limits | 512 / 256 |
| Cache count class | `0x3c20c7c8` (same identity as core header count cache) |
| Cache list payload / head / tail | 44 / 48 / 52 |
| Pixel-size call | `FT_Set_Pixel_Sizes(face, 0, pixels)` |
| Manager backing / wrapper / idle payload | 48 / 40 / 44 bytes |
| Display payload / layer / screens / screen count | 784 / 672 / 684 / 712 |
| Draw dispatch / active task | 12 / 24 |
| Pending glyph / image / gradient queues | 28 / 36 / 44; element sizes72 / 4 / 4 |

P65 keeps the same 64-byte descriptor and wrapper-specific fallback/user fields
at +28/+32. The transaction updates only the first28 bytes copied from the
backing font, preserves wrapper+36 backing membership, and checks the exact
manager live/idle owner lists before retiring anything. Native cache drop and
release always receive explicit third argument0.

P65 does not contain the P67 `lvx_vector_label` extension. Its curved/image label
widgets are distinct native classes. No P67 vector class, callback, private cache
or destructor is used; registered owners receive the standard font-property90
style refresh after publication. Draw/cache ownership checks remain active.

The old `rh_platform_font_retarget` helper is not used by this transaction. Its
native add/remove path APIs need no callable exposure: the transaction validates
and swaps the registered path node+4 itself. That unused legacy helper may stay
unsupported on P65 without disabling system-font hot reload.

## Verification

- `python3 tests/test_p65_font.py` executes the unchanged production transaction
  against P65-shaped native memory with modeled native leaves. It covers
  A→B→stock via rule removal, rejection of reused generation paths, all native
  allocation failure positions, borrowed stock faces, pending references,
  snapshot/owner changes,4096 live wrappers and511 backing records. Failed
  switching/restoration retains the incumbent generation and allows retry.
- `build/p65-tests/bin/python tests/firmware_p65_font.py` executes original P65
  Thumb instructions for the descriptor factory and count-cache initializer.
  It checks every pinned byte witness, exact descriptor callback/layout fields,
  width0/height24 pixel-size arguments, intern reference increase and cache
  list+44/head48/tail52.
- The existing `.139` and `.043` modeled production font transaction suites and
  15 target-contract/packaging tests pass after parametrization.
- Native Manager font create/release and native GPU-reference draining remain
  independently covered by `firmware_p65_features.py`.

No whole-device font rendering, graphics coherency, GPU fault/recovery,
framework restart epoch or physical-device acceptance is claimed. Those limits
match the supported healthy serialized UI transaction of the existing ports.
