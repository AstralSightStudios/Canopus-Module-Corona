# Resource-Hook Lifecycle, Internals & Verification

This document details the internal lifecycle constraints, memory layout, benchmark verification, and firmware instruction simulation for `Canopus-Module-Resource-Hook` (`ng.lst.corona`).

---

## 1. Lifecycle Boundaries & Safety Constraints

### UI Owner Serialization
- All metadata updates, cache invalidations, and owner refreshes must execute on the serialized module/UI thread through `ctx->time` callbacks. The Supervisor supplies the LVGL heartbeat; the module no longer binds native timer addresses.
- `prepare` and `activate` require the TIME service and bind the module context. Timer IDs are nonnegative (including zero); creation is followed by an explicit start, and a failed start destroys the allocation. Reactivation reuses the watcher, and completing a refresh destroys its retry timer even when the watcher finishes it. Activation errors destroy both timers and reset local IDs before the Supervisor can reclaim their entries.
- Timer delivery may pause while LVGL is suspended. Canopus does not currently recover its heartbeat after UI reconstruction; service migration does not add restart recovery or background timing guarantees.
- Theme files must remain immutable throughout the reload transaction; mutating files in-place during refresh is unsupported.
- Native setter callbacks must not delete their receiver objects or invalidate the source pointer during invocation.
- The module re-verifies object hierarchy, class, and source before each native invocation. Naked UI object pointers are never cached across UI ticks.

### File Services and Registration
- Module-owned file I/O uses the bound `ctx->fs` service for configuration, control requests/responses, font reads, calendar image banks and logs. READ opens an existing file; REPLACE uses Supervisor WRITE. Handles are module-private, including valid handle zero; failures are negative service errors, with `-2` identifying a missing file. The module does not read task errno or maintain native file I/O addresses.
- The current Supervisor implements replacement as unlink then create with mode `0666`, rather than the previous in-place truncation with `0600`. Replacement is not atomic; complete-write checks and existing retry/publication rules remain necessary.
- Registration uses the SDK module-entry helper: Band 11 passes its context to the MODULE service; Band 10 Pro stock modlib uses an atomic CMR1 message through the SDK's native transport. FS service paths exclude `/dev/canopus`. On stock modlib, constructor diagnostics remain buffered until `prepare` supplies the file-service context.
- The resource Open Hook continues to call the original native LVGL driver and return its native handle; a service FS handle must never be passed to LVGL.

### Capacity & Depth Limits
- **Object limit:** At most 1,024 objects per screen snapshot.
- **Hierarchy depth limit:** Maximum parent-chain depth of 32.
- If limits are exceeded, partial metadata refreshes are refused; requests will retry on the next tick.

### Render & Decoder Invariants
- The non-rendering flag acts solely as a UI invalidation gate and does **not** signify GPU idle.
- Deferred `VG_LITE` branches retain decoders until native pending queues are flushed. The module does not manually free decoder payloads or assume immediate closure per frame.
- Neither global cache drop nor forced synchronous refreshes are issued.

### Scope & Non-Goals
- **Watchface loader:** Not hooked. Page reconstruction is not used as a workaround. Random RAM/ROM resource replacement is not supported.
- **Animation derived owners:** Animations are not stopped globally. Derived animation classes, callback frames, canvas/snapshot buffers, and off-tree widgets require dedicated reloading protocols.
- **Process restarts:** `miwear` is not restarted automatically; `restart_miwear.sh` deliberately exits with code 78. Stopping or deactivating the module requires a clean device reboot.
- **Open hook:** The HOOK service installs `CANOPUS_HOOK_POINTER` on the exact target's LVGL POSIX open slot. Driver, original callback and slot checks remain under the publication lock. Repeated activation reuses the owned hook; a reset slot retires its old service record before reinstalling. Foreign callbacks or hook ownership conflicts are rejected. This does not make retained fonts, images or callbacks hot-unloadable.

---

## 2. Target-Specific Addresses & Cache Management

The module performs key-by-key retirement (`lv_cache_drop`) for mapped files rather than global cache clears. Only targeted keys are dropped; other paths, drivers, memory descriptors, or glyph sources remain untouched.

### Firmware Classes and Addresses

| Target | Image Object Class | Image Cache Class | Header Cache Class |
|---|---|---|---|
| `xiaomi-band-11-4.100.139` | `0x2ca14cb8` | `0x2ca168c4` | `0x2ca16944` |
| `xiaomi-band-11-4.100.155` | `0x2ca14ca8` | `0x2ca168b4` | `0x2ca16934` |
| `xiaomi-band-10-pro-3.101.043` | Target-specific AP layout validated in respective audit |

- Impacted file sources on exact image classes are verified via native `get_info`, then invalidated using native setters with the original source pointer.
- Property 40 file sources on the main part trigger explicit style refreshes without mutating style pointers or selectors.

---

## 3. Compact Rule Index & Memory Benchmarks

The module employs a compact snapshot memory layout rather than fixed-size rule tables:
- Max 256 directory prefix or exact file mappings.
- Source path up to 255 bytes; total configuration budget up to 32 KiB.
- Each snapshot entry takes 8 bytes of index metadata plus raw string storage and a 16-byte header.
- Sorted by source path; lookup first performs binary search for exact matches, then searches progressively shallower directory ancestors (longest-prefix match).
- `@system` rules indicate explicit pass-through, shadowing broader directory rules.

### Running Differential & Benchmark Tests

```sh
# Differential tests between compact index and linear parser
make -C tests test_compact # or via direct compiler invocation

# Rule benchmark execution
cc -O2 -std=c11 -Wall -Wextra -Werror -Iinclude \
  src/resource_hook.c src/config.c tools/benchmark_rules.c -o build/benchmark-rules
build/benchmark-rules
```

**Benchmark Results Comparison:**
- For a 256-rule synthetic dataset, the compact snapshot occupies **13,291 bytes**, compared to **131,072 bytes** required by the legacy fixed-capacity structures (>89% memory reduction).
- *Note:* Synthetic benchmarks measure host CPU execution; they do not include Cortex-M instruction cache effects or LVGL redraw latency.

---

## 4. Firmware Instruction Simulation & Verification

The test harness executes real firmware instructions under Unicorn emulation to verify memory layouts, cache drop logic, and object traversals without needing physical hardware.

```sh
# 1. Band 11 .155 firmware reload test
RESOURCE_HOOK_TARGET=xiaomi-band-11-4.100.155 \
  "$FIRMWARE_PYTHON" tests/firmware_reload.py

# 2. Band 11 .139 firmware reload test (extract AP from OTA first if needed)
unzip -p path/to/miwear.watch.q66tc_v4.100.139_full.bin vela_ap.bin \
  > build/firmware-analysis/vela_ap_4.100.139.bin
RESOURCE_HOOK_TARGET=xiaomi-band-11-4.100.139 \
RESOURCE_HOOK_FIRMWARE=build/firmware-analysis/vela_ap_4.100.139.bin \
  "$FIRMWARE_PYTHON" tests/firmware_reload.py

# 3. Quick-app icon read-only query and reload tests
"$FIRMWARE_PYTHON" tests/firmware_quickapp_icon.py
"$FIRMWARE_PYTHON" tests/firmware_quickapp_reload_1043.py
```

*Verification scope:* These tests validate native instruction correctness and exception safety under simulated memory conditions. They do not simulate actual hardware display pipelines or GPU completion.
