# .155 font reload: user-reported device pass

## Latest acceptance and default-build promotion

The user subsequently reports: “已实机验收，能不能把字体重载从实验性拉出来，目前固件都能用了”.
This records **USER_REPORTED_PASS** for the normal font-reload workflow on all
three supported targets, including `.155`, and authorizes promotion into the
default module. See [the current font contract](../../docs/FONT_RELOAD.md).
No per-target artifact hashes or individual restore/fault steps were supplied
with this later report. It does not independently establish GPU recovery,
restart, endurance or arbitrary-owner coverage. Newly built signed packages
are not automatically marked as individually device-tested.

## Historical 2026-09-26 checkpoint

- Recorded: 2026-09-26 (UTC).
- Device/firmware: Xiaomi Band 11, 4.100.155, as identified by the user during this session.
- Result: **USER_REPORTED_PASS** for the experimental font-replacement/reload test through Manager. A later stock-restoration attempt is **USER_REPORTED_FAIL** (`-2`, then diagnostic `-2908`); see the follow-up below.
- Evidence: after testing the wrapper-capacity fix, the user reported: “实机验证通过，记录并commit”.
- This is a user report, not an independently observed hardware trace. No screenshot, device memory dump, measured resource counts, or device-side artifact hash was supplied.

## Historical associated local artifacts

These artifacts belong to the 2026-09-26 checkpoint, not the latest all-target acceptance report. Their hashes were checked locally, not read back from the device.

| Artifact | Identity |
| --- | --- |
| Experimental module | `build/resource-hook-font-experimental.elf` |
| Module build ID | `resource-hook-0.3.0-font-exp` |
| Module SHA-256 | `4105c48f0f4031b8d139797540180b9a5265c8f45498f60febd6cfeb4fdc576b` |
| Manager | `manager/dist/ng.lst.corona.debug.1.2.2.rpk`, versionCode 5 |
| Manager SHA-256 | `58933f58a450ded2befe981b6bcfd78493b5ae694bc9d19742c04cf09bf51fd1` |
| Bundled font | `FusionPixel-12px-Proportional-zh-Hans-MiSans-Regular-subset.ttf` |
| Font SHA-256 | `4aa5e0470e672e71d76b681426e1c8300c1a5d53be4a308bcfd4db31bca17112` |
| Pinned AP SHA-256 | `ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f` |

## Device-driven fixes leading to the pass

1. Accept the 11,637,064-byte stock Regular-All font with bounded streaming validation; deduplicate reads and avoid hashing while drawing/ownership is busy.
2. Map both resource paths and the three `/tmp/MiSans-*.ttf` startup-copy paths.
3. Report revision-matched, checksummed module results in Manager instead of treating signal delivery as a font commit.
4. Correct Vela code 202 (`invalid text`): use a nonempty result placeholder; optional acknowledgement preparation must not block the original reload signal.
5. Replace generic ownership rejection `-2205` with field-specific diagnostics.
6. Address `-2702`: separate whole-list scanning from affected replacement count; scan each active/idle backing list up to 256 entries.
7. Address `-2707`: allow up to 512 affected backings, matching the combined active/idle scan capacity.
8. Address `-2762`: count and validate wrappers independently, then allocate an actual-count heap snapshot, bounded at 4096 wrappers. Preserve owner counts, fallback/user data and precommit membership checks; free snapshot memory on every exit.

Manager also uses larger 18–24 px high-contrast text, full-width buttons and a native scrolling list. The user did not separately report a formal readability/accessibility acceptance matrix.

## Supporting non-device verification

- Strict experimental ELF verification passed with zero undefined imports.
- ASan/UBSan transaction tests cover 303/1024/4096 wrappers, 4097 rejection, snapshot allocation failure/retry and changed membership.
- A 511-backing fixture covers replacement, stock restoration and early/middle/late preparation failures with old references preserved.
- Manager tests cover exact mappings, immutable generations, code-202 rejection, optional-result failure, stale/torn responses, timeout and cancellation.

These tests model native leaves; they are not full FreeType/GPU/device fault-injection tests.

## Follow-up: restoring stock fonts fails

Later in the same session the user reported: “字体替换生效之后，再按恢复默认会显示-2”. This narrows the positive result to replacement/reload only; stock restoration is now a reported failure, not merely untested. The tested build merged several preparation failures into `-2`; the user's subsequent `-2908` report on the diagnostic build identifies the preexisting-target-face refusal.

The diagnostic update separated allocation, intern-list, target-face ownership and native preparation failures into `-29xx` codes. A host fixture reproduces the now-reported `-2908` branch: successful replacement followed by refused restoration when another consumer retains the stock face. The report does not identify that device consumer or its exact reference count.

The follow-up fix allows checked reuse of unchanged, fingerprint-verified stock faces using the existing native acquisition contract. It preserves other owners, cache identities and rollback; non-stock external faces remain refused. Host tests cover repeated replace/restore, 12 fresh-face and 3 borrowed-face allocation-failure positions, pixel-size/snapshot/precommit failures, busy child caches and balanced external references. Static inspection confirms the native factory's existing-face branch skips cache construction, while glyph callbacks select the required FT size. **At that checkpoint the fixed restoration path still required device validation; these tests alone were not a device restoration pass.** The later normal-workflow acceptance is recorded above without inventing a separate restoration trace.

The hashes above identify the original reported test artifacts, **not** later rebuilt diagnostic/refactored artifacts.

## Unchanged exclusions

This pass does **not** establish GPU timeout/reset recovery, framework restart safety, unknown/custom text-owner support, arbitrary font compatibility, repeated switching endurance, memory-pressure behavior on hardware, or successful hardware stock restoration. The historical checkpoint did not enable `.139` and recorded the restoration failure above. The later all-target report supersedes normal-workflow status, not independent fault/coverage evidence.

The feature is now in the default build. Retain immutable generation files and do not bypass ownership, allocation, signature or exact-target checks. Installer builders should not automatically label newly generated artifacts as device-tested; this report records the specific user-reported test and associated local artifacts only.
