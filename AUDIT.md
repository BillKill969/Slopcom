# Finalization audit — 2026-11-11

**All four confirmed findings are resolved.** Four Luna max subagents implemented one fix each; the parent reviewed the combined changes and independently checked all four outcomes. The previous five fullscreen fixes remain covered by the passing regression suite; their history is in [FINDINGS.md](FINDINGS.md).

## Scope and result

Read all production C modules and headers, the Makefile, and relevant lifecycle/rendering/EGL/shutdown tests. Checked startup/cleanup, Xlib/XCB ownership and reply collection, Damage retirement, opacity/client discovery, shape clipping and subscriptions, full/partial rendering and history, fullscreen transitions, wallpaper lifetime, EGL import/presentation, signal/poll scheduling, and statistics.

This is a source/headless audit. It does not validate real-X appearance or provide new performance measurements. The descriptions below preserve the original failure evidence; each finding now includes its resolution and regression coverage.

| ID | Priority | Original problem | Status |
| --- | --- | --- | --- |
| A1 | P2 | Debug → release selection can leave `./slopcom` pointing at debug. | Resolved |
| A2 | P2 | Damage-region allocation failure can lose a repaint. | Resolved |
| A3 | P2 | Shape-only changes can leave fullscreen eligibility stale. | Resolved |
| A4 | P3 | Failed texture generation leaves a phantom live-texture count. | Resolved |

## A1. Build-mode switching does not reliably select the requested executable

Location: [Makefile:30](Makefile#L30).

The `slopcom` symlink is a normal timestamp-driven target. After a newer debug binary becomes its target, an older release binary does not make the symlink out of date. Requesting the release target can therefore succeed while `./slopcom` still launches ASan/UBSan code, affecting memory/performance and release validation.

Confirmed using an isolated copy of the actual Makefile and timestamped dummy sources, objects and binaries. `make BUILD=debug slopcom` links debug; the following `make BUILD=release slopcom` reports up to date and keeps the debug link. The `all` target depends on this same target. The repository binary/symlink was not altered for this reproduction.

Resolution: `slopcom` is now a phony target, so requesting it always selects the requested build mode. `tests/test_build_selection.py` uses a byte-for-byte copy of the real Makefile in an isolated fixture and checks both switching directions, repeated mode selection, and direct build-directory targets. It requires no real compiler or X/GL libraries.

Fixture: `/tmp/slopcom-audit-make-hebby_4n/`.

## A2. Failed damage-region allocation can lose a visible update

Location: `src/x11.c:1502` and `src/x11.c:1513`.

After `XDamageSubtract` clears accumulated damage, the handler relies on `parts != NULL` or `count > 0` to schedule repaint. A failed `XFixesFetchRegionAndBounds` allocation can return NULL with count zero, even when the reply's bounding box is nonempty. The fallback guarded by `count > 0` is skipped, losing the update until another repaint trigger occurs.

Verified the installed `/usr/lib/libXfixes.so.3` implementation with `objdump`: it initializes the output count to zero before reading the reply and writes the positive count only after successful rectangle allocation/read. Allocation failure returns NULL leaving that zero count; reply bounds have already been assigned. This failure contract comes from the actual library, rather than the existing successful-allocation stub.

The reproduction wraps the fetch call, preserves request/reply completion and nonempty bounds, and simulates the NULL/count-zero result. The real handler finishes with `dirty=0`, no pending repaint and zero client-damage rectangles.

Resolution: the handler now preserves nonempty bounds when rectangle allocation fails, including NULL/count-zero results. If no usable bounds exist but a positive count reports lost rectangles, it repaints the whole window conservatively. The existing `coalesced-damage` regression now covers the allocation-failure result and a successful empty result; the latter schedules no repaint. The new failure assertion was confirmed to fail against the old handler.

## A3. Client shape changes can leave fullscreen eligibility stale

Location: `src/x11.c:1169` and `src/x11.c:1285`.

The root frame is subscribed to ShapeNotify during reconciliation, but a distinct client is subscribed only after bypass entry succeeds. An initially shaped client is rejected before its subscription is installed. A subsequent change to a rectangular shape produces no client ShapeNotify for this connection. Damage events correctly avoid invalidating cached policy, so the now-eligible application can remain composited until another relevant event arrives.

Reproduction: an opaque fullscreen frame with distinct client 43 starts shaped. Policy stays composited, consumes its invalidation, and never subscribes to the client's shape. Make the client rectangular and deliver its redraw Damage: still composited. Force policy reevaluation: immediately enters bypass. A wrapper around XShapeSelectInput confirms that the missing client subscription is installed only on that successful entry.

Resolution: candidate clients are subscribed to ShapeNotify before eligibility can reject them; a failed subscription rejects the candidate. Core event subscriptions remain intact. The existing `fullscreen-policy-invalidation` case now verifies the initially shaped client, ShapeNotify-only transition to bypass, and replacement-client watch. Its existing unchanged/Damage-only no-query assertions still pass. The missing-watch assertion was confirmed to fail before the fix.

## A4. Failed texture generation leaves a false resource count

Location: `src/egl.c:325` and `src/egl.c:348`.

`egl_import_pixmap` increments `live_textures` after `glGenTextures` without checking for a nonzero texture name. If allocation fails and returns zero, import fails and releases the EGLImage, but cleanup cannot decrement the texture counter because no texture exists to delete. This is diagnostic accounting, not evidence of an actual texture leak.

Reproduction calls the real import/release code with successful image creation, failed texture generation and GL_OUT_OF_MEMORY. Observed: import false, texture zero, live images zero, **live textures one**.

Resolution: a zero texture name fails before the live counter increments or texture/image binding occurs. The created image is released. The existing EGL test executable now covers this failure and successful import/release accounting, including unchanged counters on failure and zero-name binding avoidance.

## Original audit verification and limits

- Release: `make BUILD=release all test` passed **68 Make invocations**: **67 C invocations plus 18 Python tests = 85 case executions**, with no compiler warnings.
- Debug: `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 make BUILD=debug build/debug/slopcom test` passed the same 85 case executions, with no compiler warnings or ASan/UBSan errors. Leak detection was disabled.
- A2–A4 reproduced in release and sanitizer builds using standalone harnesses and the real affected production functions. These are logic/accounting failures; they do not require sanitizer violations.
- A1 reproduced through the actual Makefile in an isolated filesystem fixture.
- Runtime artifacts: `/tmp/slopcom-final-audit-pd5ofgf5/` contains `runtime.c`, `egl-counter.c`, release/debug binaries and `reproductions.log`. Runtime cases: `client-shape-watch`, `fetch-allocation-failure`; the counter binary takes no argument. These harnesses exit zero when the bug reproduces.
- Suite logs: `/tmp/slopcom-final-audit-release.log` and `/tmp/slopcom-final-audit-debug.log`.
- No live desktop operations, source fixes, benchmarks, smoke tests, or `make check`. Real overlay/EGL handoff appearance remains unverified. Passing headless tests does not establish a bug-free release.

## Fix verification — 2026-11-11

- Combined release: `make BUILD=release all test` passed **69 Make invocations**: **67 C invocations plus two Python suites containing 19 tests = 86 case executions**. No compiler warnings.
- Combined debug: `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 make BUILD=debug build/debug/slopcom test` passed the same **86 case executions**, with no compiler warnings or ASan/UBSan errors. Leak detection was disabled.
- Independent parent rechecks: isolated release/debug/release selection chose the correct link every time; the initially shaped client was watched and entered bypass after ShapeNotify; the NULL/count-zero fetch scheduled damage; failed texture generation left both live counters at zero.
- Parent artifacts: `/tmp/slopcom-audit-fixed-recheck-7xnxbkr2/` and `recheck.log`. Combined suite logs: `/tmp/slopcom-audit-fixes-release.log` and `/tmp/slopcom-audit-fixes-debug.log`.
- No live desktop operations, benchmarks, smoke tests, or `make check` were run. Real-X visual handoff remains unverified. Existing benchmark measurements were not changed.
