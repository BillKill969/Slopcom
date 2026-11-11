# TODO5 — v0.8: Focused Code Cleanup

**Goal:** Make the existing compositor easier to change safely, without changing supported behavior or adding runtime overhead.

Use the current v0.7 source and its 42 automated test invocations as the baseline. Existing benchmark results are historical; a fresh benchmark campaign is not a prerequisite for this cleanup.

## Preserve

- Blocking idle, bounded event batches, VSync, and zero swaps when no repaint is required.
- Buffer-age history, partial drawing, presentation damage, and full-repaint fallback.
- Wallpaper updates, transparency, shapes, frame/client opacity, and override-redirect windows.
- Exact-request Damage error matching, recovery outside callbacks, and handle recreation without pixmap reimport.
- Overlay exposure repair, internal work before sleeping, and the empty-frame renderer contract.
- Current diagnostics and resource counters. Do not introduce duplicate debug-only counters.

## 1. Remove verified dead state (P0)

- [x] Remove unused `X11State.damage_history[4]` and its initialization/teardown loops. Preserve `client_history`, which is the actual repaint history.
- [x] Remove unused `XErrorTrap.end_serial`.
- [x] Check references before removing any other field or helper. Keep protocol constants and bounded limits that still serve a purpose.
- [x] Name shared limits consistently where the same invariant is currently repeated, such as event-batch capacity and buffer-history length. Preserve their values and behavior.

**Done when:** Dead state is gone, live history and counters remain correct, and the build/tests still pass.

## 2. Clarify ownership and isolate X error handling (P0)

- [x] Document ownership beside the relevant structs and lifecycle functions: named window pixmaps are owned; published wallpaper pixmaps are borrowed; GL/EGL wrappers are owned in both cases.
- [x] Keep cleanup paired and idempotent. Release imported wrappers while the EGL context is current; never free an external wallpaper pixmap.
- [x] Move the cohesive X error/capture helpers out of the large `x11.c` if this gives them a small, clear interface. Keep window discovery and reconciliation together for this milestone.
- [x] Keep Xlib's process-wide error-handler requirement explicit. Give callback context a clear install/uninstall lifetime instead of hiding it behind a generic global-state registry.
- [x] Preserve bounded serial/resource tracking, delayed-error completion, unrelated diagnostics, and deferred recovery. Callbacks must not issue X requests or mutate movable window records.
- [x] Keep required synchronization. Moving code is not a reason to remove resource-lifetime barriers or add per-Damage trap round trips.
- [x] Update Makefile dependencies and test links for any extracted source files.

**Done when:** Resource ownership and error-handler lifetime are clear from the interfaces, and existing failure/recovery tests still exercise the real implementation.

## 3. Share damage and repaint decisions (P0)

- [x] Extract the existing rectangle-list/clipping helpers into a small cohesive module if needed to share them cleanly. Keep the bounded representation and current merging behavior.
- [x] Provide one shared predicate for whether repaint work exists: explicit full invalidation or nonempty clipped damage. Main and renderer must use the same definition.
- [x] Review overlapping flags (`dirty`, `scene_dirty`, `scene_has_damage`) and remove only those made redundant by that shared contract. Keep reconciliation work distinct from repaint work.
- [x] Centralize explicit full invalidation/history reset where appropriate. Preserve startup, root-size, wallpaper, and overlay repair behavior, including damage already accumulated in the event batch.
- [x] Keep buffer-age repair and history advancement tied to actual painted frames. Empty work must not draw, swap, or advance history, including in `--full-redraw` mode.

**Done when:** There is one repaint-eligibility rule, with existing visible/hidden, exposure, empty-work, and history regressions still passing.

## 4. Consolidate duplicated drawing and narrow headers (P1)

- [x] Share window visibility checks, texture/uniform setup, and shaped-window drawing between full and partial repaint paths where doing so removes duplication.
- [x] Pass the active clip explicitly. Full repaint uses the viewport; partial repaint intersects shape rectangles with the repair rectangle and restores the outer scissor afterward.
- [x] Preserve stacking, premultiplied alpha, wallpaper coverage, and the distinct buffer-age/presentation bookkeeping of full and partial frames.
- [x] Reduce unnecessary header dependencies with forward declarations or a small shared types header where useful. Do not create a second authoritative window/resource representation.
- [x] Keep interfaces cohesive: pass the state a function actually owns or needs, without replacing it with long lists of individual fields.
- [x] Add short contract/ownership comments at changed interfaces. Reuse existing documentation rather than creating a large architecture manual.

**Done when:** Common drawing behavior has one implementation, and full/partial paths remain easy to follow with their clipping differences explicit.

## 5. Short verification and completion

- [x] Keep refactors small and behavior-preserving. Run focused existing cases after each meaningful step; do not add tests that merely mirror a file move.
- [x] Retain coverage for delayed visible/hidden Damage errors, exact matching, creation failure, recovery without reimport, exposure, pending work before `poll()`, and statistics boundaries.
- [x] Add a targeted regression only for a newly exposed boundary that existing tests do not cover, especially mixed visible/hidden damage or shape clipping during partial repaint.
- [x] Build release and run `make test`. Run one ASan/UBSan suite after final ownership/error-path changes; document the existing LeakSanitizer tracing limitation if leak detection must be disabled.
- [x] If scheduling/drawing changed and a suitable X session is available, use a brief affected-path visual check and about 10 seconds of settled idle. Record unavailable checks for daily driving.
- [x] Record actual results and any deferred extraction in `docs/validation.md`. Keep benchmark claims tied to the build and workload that produced them.

**Completion:** The selected cleanup is implemented, source builds cleanly, the existing suite and relevant new regressions pass, and supported behavior is preserved. Do not claim performance or RAM improvements from moving code between files.

## Out of scope

- A six-layer rewrite, new framework, event/message bus, threading, or backend abstraction.
- RandR hotplug, per-output scheduling, mixed-refresh guarantees, or new suspend/context-loss recovery.
- Root painting without published wallpaper pixmaps, fullscreen unredirection, effects, or new configuration.
- New profiling infrastructure, telemetry/schema redesign, speculative CPU/RAM tuning, or packaging work.
- Mandatory full application matrices, extended desktop sessions, soak tests, or battery benchmarks.

**Order:** Dead state → ownership/error handling → shared repaint decisions → common drawing helpers → short verification. Skip a proposed extraction if it only moves complexity behind more interfaces; document why and keep the simpler arrangement.
