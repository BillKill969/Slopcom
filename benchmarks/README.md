# slopcom Benchmark & Verification Suite

This directory contains benchmark harnesses, telemetry monitors, verification suites, and long-run soak runners for `slopcom`.

---

## 1. Statistics & Resource Counter Definitions

All counters in `slopcom` are tracked internally in `Stats` ([`src/stats.h`](../src/stats.h)) and exported via JSON Lines on `SIGUSR1` and exit.

| Counter Name | Definition & Measurement Points | Lifetime / Reset Semantics |
|---|---|---|
| `x_events` | Total number of Xlib events received and processed by `x11_handle_event`. | Monotonically increasing from startup. |
| `damage_events` | Number of `XDamageNotify` extension events delivered. | Monotonically increasing from startup. |
| `render_attempts` | Number of dirty frames where `render_scene()` was executed. | Monotonically increasing; must NOT increment during idle. |
| `successful_swaps` | Successful regular or damage-aware presentation calls. | Monotonically increasing; must match painted frames when presentation succeeds. |
| `full_repaints`, `partial_repaints` | Actual full or partial renderer branches taken. | Increment once per painted frame, not for empty work. |
| `poll_wakeups` | Positive readiness returns from the application's `poll()` call. | Includes signal readiness; distinct from OS context switches. |
| `skipped_occluded_damage` | Damage acknowledgements skipped for fully occluded windows. | Counts updates, separately from skipped event batches. |
| `skipped_presentations` | Processed event batches that produced no scene damage or geometry change after reconciliation (e.g. duplicate configures or unmapped window updates). | Monotonically increasing; does not increment while blocking idle. |
| `imports` | Confirmed initial pixmap/texture GPU imports for new viewable windows (`eglCreateImageKHR` + `glEGLImageTargetTexture2DOES`). | Monotonically increasing from startup. |
| `reimports` | Confirmed storage reimports for existing windows whose backing pixmap changed (resize or unmap/map storage transition). | Monotonically increasing; moving a window preserves import and does NOT increment. |
| `x_round_trips` | Selected instrumented Xlib/XCB query/synchronization operations. Omits some `XSync()` calls; not a packet-level measurement or batching efficiency metric. | Monotonically increasing from startup. |
| `live_pixmaps` | Currently allocated server-side named XComposite pixmaps (`XCompositeNameWindowPixmap`). | Increments on creation, decrements on `XFreePixmap`. **Must be exactly 0 at teardown.** |
| `live_images` | Currently allocated `EGLImageKHR` objects (`eglCreateImageKHR`). | Increments on creation, decrements on `eglDestroyImageKHR`. **Must be exactly 0 at teardown.** |
| `live_textures` | Currently allocated GLES2 textures (`glGenTextures`). | Increments on creation, decrements on `glDeleteTextures`. **Must be exactly 0 at teardown.** |
| `live_damage` | Currently active XDamage handles (`XDamageCreate`). | Increments on creation, decrements on `XDamageDestroy`. **Must be exactly 0 at teardown.** |
| `live_regions` | Currently allocated server or shape regions (`XShapeGetRectangles`). | Increments on allocation, decrements on `XFree`. **Must be exactly 0 at teardown.** |

### Timing Distribution Metrics (`timing_cpu_ns`)

Available when running with `--stats`:
- `event_reconcile`: Elapsed nanoseconds for event draining, Damage completion, and scene reconciliation per batch; excludes the loop's preceding idle poll.
- `draw`: Elapsed nanoseconds around GLES draw submission (`render_scene`).
- `swap`: Elapsed nanoseconds around regular or damage-aware presentation.
- The legacy `timing_cpu_ns` key contains `CLOCK_MONOTONIC` intervals, including X server or driver waits. It does not measure GPU execution time or thread CPU time. The fixed-size timing ring can include startup and earlier frames; its percentiles are not limited to the latest benchmark interval and cannot be subtracted to obtain interval percentiles.
- Each bucket reports sample count, p50, p95, and p99 percentiles.

---

## 2. Process Memory Diagnostics (`memory` subcommand)

The `benchmarks/run.py` harness provides on-demand memory inspection without requiring an active X11 display or interfering with a running compositor:

```sh
# Read settled memory snapshot for a process (integer KiB)
python3 benchmarks/run.py memory --pid <PID>

# Also include VMA mapping attribution from /proc/<pid>/smaps
python3 benchmarks/run.py memory --pid <PID> --mappings
```

### Operational Invariants & Semantics
- **Display Check Bypass**: Dispatched immediately after argument parsing before any X connection, display checks, or compositor selection ownership tests. Works headless or while a compositor owns `_NET_WM_CM_S0`.
- **Identity & Recycling Guard**: Verifies process start ticks from `/proc/<pid>/stat` before and after file reads to ensure measurements are not mixed across recycled PIDs.
- **Strict Missing-Data Semantics**: Missing, unreadable, negative, or malformed values remain `None` (JSON `null`) with descriptive reasons in `unavailable`, never silently defaulting to zero.
- **Reporting Schema (v1)**:
  - `rss_kib`: Process Resident Set Size from `/proc/<pid>/smaps_rollup`.
  - `pss_kib`: Proportional Set Size (shared pages apportioned across sharing processes).
  - `private_rss_kib`: `Private_Clean + Private_Dirty` (only computed when both are valid).
  - `shared_rss_kib`: `Shared_Clean + Shared_Dirty` (only computed when both are valid).
  - `pss_anon_kib`, `pss_file_kib`, `pss_shmem_kib`: Optional PSS breakdown.
  - `swap_kib`, `private_hugetlb_kib`: Reported separately from resident RAM.
  - `mapping_groups`: When `--mappings` is passed, aggregates VMAs by label/path, sorted descending by RSS, with PSS and private RSS alongside.

---

## 3. Standard Workloads

1. **`empty-idle`**: Undisturbed desktop with no fixture windows. Checks render/swap deltas over the selected sample interval.
2. **`overlapping-idle`**: Two overlapping static fixture windows (one 24-bit RGB with asymmetric quadrants, one 32-bit translucent ARGB). Tests transparency composition without animations.
3. **`typing`**: Fixture updating a simulated typing cursor at 8 Hz in a text-like area. Measures low-frequency localized damage.
4. **`drag-resize`**: Fixture actively resizing and moving between geometries at 20 Hz. Tests window management overhead.
5. **`scrolling`**: Fixture window scrolling contents vertically at 30 Hz. Tests continuous partial scene updates.
6. **`video`**: 60 Hz full-window animation fixture. Measures that update cadence and presentation behavior, not maximum throughput.

---

## 4. Fast Verification & Benchmark Presets

Harnesses support tiered execution presets:

### Fast Verification & Test Targets
- `make test`: Executes **69 Make invocations**: 67 C regression cases and two offline Python suites containing **19 tests** (18 memory tests and one build-selection test; **86 case executions** total). No X session is required; desktop/process/power helpers are isolated in workload tests.
- `make check`: Runs `make test` followed by `python3 benchmarks/run.py verify --quick` (selection collision, 10 start/stop cycles, 5 real-X fixtures, scripted churn, and i3 interactions) in **<12s**.
- `python3 benchmarks/smoke_wallpaper.py`: Runs real-X wallpaper compatibility smoke tests (`xwallpaper` startup, hot-changes, tiling, `--clear`, feh fallback, and 10s idle invariant).
- `python3 benchmarks/smoke_apps.py`: Runs real-X app compatibility smoke tests (`dunst` notifications, `dmenu` launcher, `xterm`, `alacritty` transparency, `i3bar`).
- `python3 benchmarks/run.py verify --test <collision|cycles|fixtures|churn|i3>`: Runs isolated test targets on demand (e.g., `--test collision`, `--test cycles`, `--test fixtures`, `--test i3`).

### Benchmark Execution Presets
- `make smoke` (`python3 benchmarks/run.py bench --smoke`): Runs all 6 workloads on available backends with 0.5s settle and 1.5s sample in **~25s total**.
- `make bench-quick` (`python3 benchmarks/run.py bench --quick`): Runs all 6 workloads with 1.0s settle and 3.0s sample in **~45s total**.
- `python3 benchmarks/run.py bench --fast`: Runs all 6 workloads with 2.0s settle and 5.0s sample in **~1m total**.
- `python3 benchmarks/run.py bench --gate`: Runs formal milestone verification with 3 repetitions, 30s settle, and 60s sample.
- Target individual workloads: `python3 benchmarks/run.py bench --workloads video --smoke`.
- **Memory Snapshots Opt-In (`--memory`)**: Passing `--memory` records `memory_before` and `memory_after` snapshots (schema v1) for the compositor process, captured after settling and after monitor shutdown. Unsampled backends (e.g. `none`) leave these fields `null`.

The `memory` command only inspects the selected process. Live `bench`, `verify`, smoke, and soak commands can start processes or interact with the desktop and need a suitable test session. They are not required for routine documentation or parser changes. See [v0.9 closeout](../docs/DONE6.md) for measured results and deferred runtime optimizations.

---

## 5. Long-Run Stability & Resource Soak Tests

The `soak` subcommand executes continuous multi-window churn, workspace migrations, and real-time telemetry monitoring:

```sh
# Quick 60-second smoke soak test
python3 benchmarks/run.py soak --quick

# Full 4-hour soak test
python3 benchmarks/run.py soak --duration 14400 --sample-interval 30
```

During execution, it samples `/proc/<pid>/status` (`VmRSS`), `/proc/<pid>/fd` (open file descriptors), CPU usage, and available GPU telemetry. Growth checks and selected resource counters can flag regressions; they do not account for every driver/server allocation or prove absence of leaks. Extended runs are optional investigations.
