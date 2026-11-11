# Fullscreen unredirection findings and dispositions — 2026-11-10

The later finalization audit found four additional issues, now resolved; see [AUDIT.md](AUDIT.md). The five findings below describe the earlier fixes.

Five runtime findings were confirmed by focused headless reproductions. The first three were reported in the initial review; findings 4–5 are additional. This follow-up implements and covers all five findings.

## 1. Consumed events cancel newly entered bypass

Location: `src/x11.c:1214`, `src/x11.c:1313`.

Map/configure events set `fullscreen_recheck`. Normal reconciliation consumes their metadata changes, and policy enters bypass, but entry leaves that flag set. On the next loop iteration, it requests restoration even with no new events.

Reproduction: deliver MapNotify, reconcile, enter bypass, then advance policy without another event. Observed modes: `BYPASSED` with `recheck=1`, then `RESTORE_HIDDEN`.

Fix: consume the policy invalidation associated with the evaluated scene, while preserving invalidations from events arriving during synchronization. Verify that a newly entered, unchanged candidate stays bypassed and reaches poll. This proves an unnecessary restore; it does not independently prove an endless transition loop.

Disposition: resolved. Initial policy evaluation is scheduled when the option is enabled. Evaluated invalidations are consumed, while queued X events and pending scene work keep the invalidation pending. An unchanged candidate remains bypassed.

## 2. Failed entry allows a frame before storage recovery

Location: `src/x11.c:1182`, `src/x11.c:1197`, `src/main.c:340`.

Entry releases all window storage before attempting unredirection/hiding. Both recoverable failure paths set `pending_reconcile`, schedule a repaint and return success. Main already reconciled this iteration and proceeds directly to rendering. Required window textures are absent until the following iteration.

Reproduction: separately inject unredirection failure and overlay-hide failure, then run the actual renderer and presentation stub in main's order. Both produce `pending_reconcile=1`, **zero client draws, one full background clear and one presentation**.

Fix: complete storage recovery before allowing rendering/presentation, or explicitly defer drawing until recovery reconciliation has completed. Cover both failure branches.

Disposition: resolved. Both recoverable entry failures synchronously reconcile storage and return a complete dirty scene before rendering can continue; unrecoverable recovery returns failure.

## 3. Unchanged composited frames repeat synchronous eligibility queries

Location: `src/x11.c:1299`, `src/x11.c:1315`.

The composited path evaluates eligibility on every loop iteration, without checking whether relevant state changed. Client discovery and property reads contain synchronous error-trap barriers. The bypassed path also queries eligibility on unrelated wakeups when no invalidation is pending.

Reproduction: keep a fullscreen application composited with bypass hint 2, consume the initial policy evaluation, then invoke policy ten times with unchanged state. Observed **40 additional counted round trips and 100 XSync calls**. These are request counts, not a measured frame-time regression.

Fix: evaluate policy initially and after relevant invalidations, retaining the result across ordinary Damage events and statistics-only wakeups. Ensure deferred policy work is included in main's internal-work predicate where necessary. Preserve event-driven restoration and both restoration phases.

Disposition: resolved. The policy only queries on initial or relevant invalidation. Main schedules the initial evaluation when enabled, and unchanged composited or bypassed iterations do not query eligibility.

## 4. Restoration can reveal a scene missing a live window

Location: `src/x11.c:904`, `src/x11.c:1276`.

The pixmap naming/geometry failure branch drops a window and continues, treating capture failure as proof that the window vanished. Restoration accepts reconciliation success as proof of a complete scene. Unlike the EGL-import failure branch, this earlier branch never verifies whether the window remains viewable.

Reproduction: enter bypass, inject pixmap geometry failure during restoration, and confirm the application still exists and is viewable through XGetWindowAttributes. Restoration returns success with `num_windows=0`; the actual renderer draws no client, and the completion hook reveals the overlay. The discarded record also lacks Damage tracking, so ordinary application painting cannot repair its omission.

Fix: distinguish vanished/unmapped windows from capture failures for still-viewable windows. Do not advance restoration to reveal an incomplete scene. Handle unrecoverable capture failure through existing cleanup, or use a bounded recovery that preserves the hidden overlay until a complete scene is available. Check other reconciliation paths that return success without establishing scene completeness, including skipped root-tree queries.

Disposition: resolved. A failed pixmap capture is checked against current viewability. Required full-scene reconciliation also fails on root-tree errors, unavailable visuals, unexpected XCB errors, and unexplained missing XCB replies. Failed restoration keeps the overlay hidden until cleanup.

## 5. Candidate intersection counts borders twice

Location: `src/x11.c:1097`.

`WindowState.width/height` already include borders, both from imported pixmap dimensions and metadata-only reconciliation. Candidate selection adds `2 * border_width` again when calculating right/bottom bounds.

Reproduction: place an override-redirect popup above an otherwise eligible application. Give the popup client width 80, border 20 and x=-130. Its actual outer width is 120 and right edge is -10, entirely offscreen. Policy calculates a right edge of 30 and incorrectly lets it block bypass.

Fix: calculate the outer bounds using the established width/height contract once. Cover the offscreen bordered case in both composited and metadata-only states.

Disposition: resolved. Candidate intersection uses stored outer dimensions once; the bordered offscreen popup regression covers both normal and metadata-only reconciliation.

## Documentation and coverage

README.md, TREPORT.md, KNOWN_LIMITATIONS.md and the validation section previously described a guaranteed zero-flicker handoff. Those guarantees have been replaced with the intended two-phase ordering and an explicit note that real-X handoff remains unverified. README.md's test count has been updated.

The targeted regressions now cover policy invalidation/gating, failed entry through the actual renderer and presentation stub, bordered popup intersection in both reconciliation modes, and strict restoration failures.

## Verification evidence

- Initial review: release and ASan/UBSan suites passed 65 C invocations plus 18 Python tests, 83 case executions per configuration. No compiler warnings.
- Follow-up: six focused scenarios reproduced against the real X11 policy/reconciliation and renderer, with offline X/GL/EGL stubs: event entry, failed unredirect, failed hide, unchanged composited policy, live-window capture-geometry failure, and offscreen bordered popup.
- The same six scenarios reproduced with ASan/UBSan and no sanitizer errors. Leak detection was disabled; this does not establish absence of leaks.
- Temporary reproduction source/binaries: `/tmp/slopcom-fullscreen-followup-v71pna5u/`; sanitizer log: `sanitizer-repros.log`. Each scenario exits 1 when the bug reproduces.
- Final `make BUILD=release all test` and `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 make BUILD=debug build/debug/slopcom test` both passed 68 Make invocations (67 C cases plus one Python suite with 18 tests; 85 case executions). The first release attempt stopped on a counter-baseline error in the new test; after correcting that fixture, the release suite passed.
- The parent independently reran six release repros after the fix; all passed. Source/log: `/tmp/slopcom-fullscreen-parent-recheck.c` and `/tmp/slopcom-fullscreen-parent-recheck.log`.
- No live desktop operations, benchmark runs, smoke tests, or `make check` were run.
