# v0.9 — Memory diagnostics and measured scope

**Diagnostics and reporting are complete; runtime optimization remains deferred.** The milestone changed Python tooling, tests, and documentation. No runtime C optimization was implemented or demonstrated by the available measurements.

## Delivered

- `python3 benchmarks/run.py memory --pid PID [--mappings]` reads process memory without connecting to X or changing the desktop. It validates process identity across reads and reports unavailable values as JSON `null` with reasons.
- `bench --memory` optionally captures memory before and after monitoring, outside the timed monitoring interval. It adds no compositor timers or periodic `smaps` reads.
- Missing process RSS no longer becomes a false zero. Offline tests cover parsing, missing data, PID replacement, command dispatch, and memory opt-in.
- Workload tests isolate workspace, subprocess, monitoring, power, and GPU helpers, so the offline suite does not manipulate the desktop.
- Reports distinguish OS context switches, application poll returns, selected instrumented Xlib operations, and elapsed timing intervals.

## Recorded memory

The [settled release snapshot](../benchmarks/results/memory_attribution_settled.json) was recorded on Radeon 780M/Mesa with i3 at 2880 × 1800. Its command was `./build/release/slopcom --stats :1`; the artifact does not retain a window count.

| Metric | Recorded value |
| --- | ---: |
| RSS | 154.7 MiB |
| Shared resident pages | 137.9 MiB (89.1% of RSS) |
| Private resident pages | 16.9 MiB |
| PSS | 41.6 MiB |

RSS includes large LLVM and Gallium mappings. Private pages also include libraries and drivers; they are not a measurement of slopcom's own bookkeeping. Mapping labels do not establish allocation ownership. RSS and PSS are overlapping views, and neither accounts completely for X server or GPU allocations.

A separate [two-second empty-idle sample](../benchmarks/results/bench/1794229930_slopcom_empty-idle_1.json) recorded zero renders, swaps, poll wakeups, and OS context switches. RSS, private RSS, and PSS each rose **8 KiB** between its endpoints. This short observation establishes neither sustained growth nor absence of leaks.

The active-workload table in [bench.md](../bench.md) is historical, from separate 1.5-second fixtures at 2880 × 1760. It is not a fresh v0.9 performance comparison or maximum-throughput measurement.

## Verification

- Latest release checks passed: **45 Make invocations**, consisting of **44 C regression cases** and one offline Python suite with **18 tests**.
- Earlier v0.8 C checks passed ASan/UBSan with leak detection disabled for the tracing environment. Those checks were not repeated for the subsequent Python/docs-only correction and do not establish absence of leaks.
- Existing live observations are retained in the [validation record](validation.md). No additional live desktop checks or extended soak runs are required for these documentation changes.

## Deliberately deferred

Active-workload reconciliation and presentation-damage allocation costs have not been quantified. The idle sample does not justify changing either path. No lower-RAM, lower-power, or improved-latency claim follows from this milestone.

Use daily driving to identify reproducible bugs or meaningful overhead. If one appears, profile the affected path, make a focused fix, run the relevant existing regressions, and compare matching workloads when claiming a performance improvement. Investigate lifecycle memory growth only when there is evidence to explain.

---

## Four Focused Rendering Optimizations (2026-11-09)

Following authorization in [`PLAN.md`](../PLAN.md), four bounded rendering optimizations were implemented and verified across headless regression suites:

1. **Opaque Blending Bypass** (`src/render.c`, `tests/rendering.c`):
   - In `draw_scene_windows()`, blending is dynamically disabled (`glDisable(GL_BLEND)`) for windows with `!has_alpha && opacity == 1.0f`.
   - Blending remains enabled for ARGB windows (even at opacity 1.0) and near-one opacity windows. Painter's order and premultiplied blending are preserved.
   - Tested by `opaque-blending-bypass` in `tests/rendering.c` (covers full/partial redraws, transitions between opaque and translucent windows, and ARGB at opacity 1.0).

2. **Bounded Presentation-Damage Storage** (`src/egl.h`, `src/egl.c`, `src/render.c`, `tests/rendering.c`):
   - Replaced dynamic heap pointer in `EGLState` with inline `EGLint presentation_damage[4 * MAX_DAMAGE_RECTS]`.
   - Completely eliminated per-frame `realloc()` and cleanup `free()` on the presentation path.
   - Tested by `bounded-presentation-damage` in `tests/rendering.c` (verifies full repaint count reset, max-capacity 32-rect damage, coordinate conversion, and single-rect damage).

3. **Buffer-Efficient EGL Config Selection** (`src/egl.c`, `tests/egl_config.c`, `Makefile`):
   - Added attribute-validated candidate checking (`egl_is_config_eligible` and `egl_select_best_config`) to query candidate configs, enforcing exact native overlay visual matching, GLES2 bit, window surface bit, and requested swap interval capability (0 for `--no-vsync`, 1 otherwise).
   - Ranks compatible candidates by fewest multisample samples, then shallowest depth buffer, then smallest stencil buffer.
   - Logs selected attributes on startup; preserves compatible fallback if zero-extra-buffer configs are unavailable.
   - Tested by dedicated headless harness `tests/egl_config.c` (checks rejection of visual mismatch, non-GLES2, non-window surface, invalid swap intervals, attribute query failures, fallback ranking, and tie-breaking).

4. **Reusable Quad Geometry & Vertex State** (`src/render.c`, `src/render.h`, `tests/rendering.c`, `tests/background.c`, `Makefile`):
   - Replaced streaming six-vertex CPU uploads with a single unit quad VBO uploaded once in `render_init()` with `GL_STATIC_DRAW`.
   - Window positions, dimensions, and texture UV extents are passed as uniforms (`u_rect`, `u_uv_extent`) in `k_vertex_shader_src`.
   - Vertex attributes are bound in `render_init()` and reused across frames.
   - Preserves scissor clipping, shaped window rects, borders, negative coordinates, and wallpaper repeat extents.
   - Tested by `static-geometry-reuse` in `tests/rendering.c` (asserts zero `glBufferData` uploads during scene rendering, correct `u_rect` transforms, and identical geometry reuse across shape clips) and `tiny-tile`/`tiling-quads` in `tests/background.c`.

### Verification Results

- `make BUILD=release all test`: **49 test invocations** (48 C regression cases + 1 Python suite with 18 tests = **66 tests total**), all **PASS** (0 warnings, 0 failures).
- `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 make BUILD=debug test`: All **PASS** (0 errors). Leak detection was disabled due to the tracing environment limitation and does not establish absence of leaks.
- Reductions in API and allocation overhead are bounded and verified by tests; no speculative FPS, RAM, or battery improvements are claimed.

