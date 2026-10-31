# Corona Manager Mirage fixtures

These fixtures target the real `.043` / `.155` firmware in Mirage. They are not host-rendered UI screenshots and do not establish device performance or production approval.

Generate a fresh module-private seed tree, then run one process per target with **different** library and evidence directories:

```sh
python3 tests/fixtures/manager-emulator/generate.py 155 /tmp/corona-seed-155
python3 scripts/test-manager-emulator.py 155 \
  --library /tmp/corona-manager-155-library \
  --work /tmp/corona-manager-155-evidence \
  --package /absolute/path/to/canopus_corona-0.3.0.canopus \
  --firmware-source /absolute/path/to/miwear.watch.q66tc_v4.100.155_full_f1c824fe \
  --seed-dir /tmp/corona-seed-155 \
  --scenario tests/fixtures/manager-emulator/155.json
```

For `.043`, use `043`, its own library/work/seed, and the local `miwear.watch.p67tc_v3.101.043_full_a4ce8564` firmware directory. `MIRAGECTL`, `MAB_SMOKE`, `FACETORY_PACKAGER`, `MIRAGE_QEMU` and `CANOPUS_ROOT` override tool/check-out discovery. `MAB_SMOKE` must use the current native Canopus Core API and package limit; a bridge accepting only `watchface` / `quickapp` is insufficient. The harness pins the input package to `evidence/corona-input.canopus` and installs that copy, recording module, bridge and QEMU hashes. Production installer resources default to the matching family in `Canopus-Private`; `--installer-dir` accepts an explicitly prepared directory. The harness does not build dependencies or edit that checkout.

The seed contains forty synthetic installed themes, one system divider, forty-one resource paths, two explicit mixed choices, and a protected first package. Tiny preview images are procedural LVGL v9 ARGB8888 tiles; no firmware resource images are included. The real target calendar path is a resource key so the default/theme preview can be compared against the original system image. Fixture generation writes only the requested empty output directory.

The 86-step scenarios cover all six routes, the delete confirmation page (the seed's active first package renders its reason and no action; a second tap at the same spot proves nothing runs), held dragging and restoration, lists beyond the 32-node snapshot limit, preview selection, internal reload with its status-card result layer, and six transfer-banner views (phases 1–6 pinned above the resources list; the idle phase renders no banner and no waiting page by design). `drag` first holds still for 800 ms, then moves through Mirage's local VNC socket; a linear `miragectl swipe` starts moving immediately and does not reliably trigger the UI's long-press threshold. Stationary 700 ms holds use the test-only transfer-phase cycle on the home card: each hold advances one phase and the banner states are captured from the resources page; the finished install is captured on the home page, where no toast remains because Canopus removed the toast API. Screenshots, scenario progress and hashes land in `run.json` and `screenshots/`.

**The coordinates are implementation-derived and require firmware validation.** The scenarios require a module built with the compile-only `RH_MANAGER_UI_TEST` hook and bounded snapshot at `m.corona/ui-probe.txt`. Final assertions require `routes_mask=63`, `phases_mask=127`, `render_failures=0`, `notice=0` (a reload result must not outlive its dwell), the receiving-68 banner marker, and nonzero reload/drag/delete/pick/scroll counters, where `delete` counts confirmation pages opened rather than deletions (a referenced package has no action to press). The fixture deliberately performs two drags to restore the initial ordering. Transfer is driven through the internal state setter from the serialized UI owner after touch; no Connect listener or DATA messages are used. Calling the internal state functions on an arbitrary stopped CPU does not establish the UI-owner contract.

A package without the test hook fails the probe assertion; it is not silently accepted. Omitting `--scenario` only captures the opened home page and reports `captured`, never `passed`. Missing tools, firmware or module artifact produce exit 77. Failed installation, page opening, image assertion or probe checks fail the run. A human must inspect the produced screenshots for visual acceptance; the harness records that visual review has not been performed.
