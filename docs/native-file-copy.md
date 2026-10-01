# Manager native file-copy audit

This audit supports using `@system.file.copy` for Manager overlay materialization.
It is static firmware analysis, not a device throughput measurement or a proof of
crash/power-loss atomicity. The copy API is used directly; Manager has no legacy
JS chunked-copy fallback.

## Evidence

Analyzed with IDA/Hex-Rays:

- Xiaomi Band 11 `4.100.139`: `build/firmware-analysis/vela_ap_4.100.139.bin.i64`.
  Firmware SHA-256: `31ce82257f7c127950dc5070b86316730cf468a41f0d004559e41e7d923b2c74`.
- Xiaomi Band 11 `4.100.155`: `build/firmware-analysis/vela_arm_mapped.i64`,
  containing the mapped `vela_ap_4.100.155.bin` image. The separately named
  `vela_ap_4.100.155.bin.i64` could not be opened by the IDA worker, so it was not
  used as evidence. Selected code bytes were also compared against the raw
  firmware. Firmware SHA-256:
  `ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f`.

Runtime code addresses below are the same on both analyzed versions. Some string
and reflection-table addresses differ by 16 bytes; do not transpose them blindly.
No firmware bytes were patched.

### Binding and dispatch

The `copy` reflection entry is:

| Firmware | Name pointer | Reflection descriptor | Native handler (Thumb) |
| --- | --- | --- | --- |
| 4.100.139 | `0x2cbeba0c` (`copy`) at `0x0ca7ea1c` | `0x2ca7dee0` | `0x0c4ac97b` |
| 4.100.155 | `0x2cbeb9fc` (`copy`) at `0x0ca7ea0c` | `0x2ca7ded0` | `0x0c4ac97b` |

Handler `0x0c4ac97a` invokes `0x0c4a9ba8` (`file_copy_or_move`) with operation
selector zero. The request uses `srcUri`, `dstUri`, `success`, `fail`, and
`complete`. The selector-zero branch constructs a libuv filesystem request with
operation 29 (`UV_FS_COPYFILE`) and flags zero, duplicates the native path strings,
and submits worker `0x0c3f5964` through `0x0c3f4cc4`. It does not perform JS-side
read/write chunks.

### URI resolution

`0x0c49a150` translates `internal://files/<relative>` using the current app ID to
`/data/quickapp/files/<app-id>/<relative>`. `0x0c49a24c` rejects tmp destinations
for this operation. Manager must continue supplying `internal://files/` URIs,
not native absolute paths. Existing safe-relative-path and expanded-path-length
validation remains mandatory.

### Copy behavior

The `UV_FS_COPYFILE` worker dispatches to `0x0c3f63d0`:

1. Open and stat the source.
2. Open/create the destination. With flags zero, an existing destination is
   allowed rather than opened exclusively.
3. For distinct source/destination paths, truncate the destination to zero via
   `0x0c33e9dc` before copying. Failure to truncate is an error (apart from the
   worker's special handling of an already-empty target).
4. Transfer source bytes using the native sendfile path, repeating until the
   source stat size is consumed.
5. Close both descriptors. Close failures participate in the final result.
6. After a failure with an opened destination, attempt to unlink that destination.

There is no recursive parent-directory creation and no atomic rename/publication
of the destination. Manager therefore prepares each distinct destination parent
once and copies only into a new, unpublished `.active-<revision>/` generation.
It waits for every native copy to succeed before changing the active TSV; Manager
no longer stats every source and destination. A failure removes the new generation; old generations are retained until a
matching successful module receipt.

The worker's internal sendfile implementation can fall back to native buffered
I/O. This remains inside firmware and is unrelated to a Manager JS fallback.

### Completion and errors

Callback handler `0x0c4a9ac8` checks the completed filesystem result before
invoking success with the translated destination URI. It invokes fail for negative
results, then complete, and releases the retained callbacks, instance reference,
and filesystem request through `0x0c4a9a00`.

Error mapping `0x0c4a1dbc` is:

- `-EINVAL` (`-22`) -> `202`;
- `-ENOENT` (`-2`) -> `301`;
- other native filesystem errors -> `300`.

The wrapper also has exceptional allocation-failure paths that only log, and it
suppresses callbacks after the owning feature instance detaches. These are
pre-existing runtime behaviors, not guarantees of completion under all failures.
The Manager's app-owned adapter reduces dependence on a transient page instance.
No timeout that could race an ongoing native copy is introduced in this change.

## Manager checks

- Reload, receiver packet handling, and package deletion share an app-owned operation
  queue across independently bundled pages. Controlled package writes cannot change
  the fresh snapshot during reload preparation or while awaiting its module receipt.
  Receiver writes/deletions wait until that operation completes; the receipt polling
  interval and attempt budget are unchanged.
- Overlapping directories use exact-file mappings when the complete TSV fits the
  256-rule / 32 KiB budgets; immutable materialization is the fallback.
- The app-owned package snapshot retains the last successfully published plan.
  Unchanged order and overrides reuse it without new copies or a TSV rewrite.
  Receiver/package deletion invalidation replaces the snapshot and its plan;
  application restart also drops this memory-only cache. Every reload still sends
  a unique request revision and awaits its matching receipt before cleanup.
- No per-asset source/destination stat or content hash is performed. Inventory
  validation still rejects malformed metadata; copied empty assets are rejected.
- Native errors are propagated, never reinterpreted as API unavailability.
- All native copies finish before `mappings.tsv` is written or a reload request
  is sent. Failure removes only the new unpublished generation.
- The existing generation registry also persists `protectedThemes`: the union
  of prior/candidate direct-package dependencies is written before the active TSV.
  Delete/replace guards consult both the TSV and this registry, failing closed on
  malformed metadata. Only matched successful acknowledgement narrows protection
  to the acknowledged plan, including after a failed switch or app restart.
- Directly mapped active packages cannot be replaced/deleted until acknowledged
  switch-out. External file edits are unsupported; power-loss consistency is
  outside this audit's guarantees.

Official contract:
https://iot.mi.com/vela/quickapp/zh/features/data/file.html#file-copy-object
