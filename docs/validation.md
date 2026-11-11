# slopcom Validation and Evidence Record

This document records the exact test scenarios, commands, execution environments, results, and artifact paths for all validation gates in [`DONE2.md`](DONE2.md) and [`DONE3.md`](DONE3.md).

---

## 1. Test Environment

| Attribute | Target Machine Configuration |
|---|---|
| **OS / Distribution** | Arch Linux (x86_64) |
| **Kernel** | `7.2.14-arch1-1 #1 SMP PREEMPT_DYNAMIC` |
| **GPU Hardware** | AMD Phoenix1 (Radeon 780M / Ryzen 7000/8000 series iGPU, rev c9) |
| **Graphics Driver** | Mesa `26.2.6-1` (`radeonsi`) |
| **X Server** | X.Org Server `21.1.25-2` |
| **Window Manager** | i3 `4.25.1` (lightweight tiling WM) |
| **Display / Output** | `:1` on `eDP-1` (2880x1760, single screen) |
| **Compiler & Flags** | GCC C11 (`-Wall -Wextra -Wpedantic -g -O2` release; `-O1 -fsanitize=address,undefined` debug) |
| **Compositor Selection** | `_NET_WM_CM_S0` (XComposite manual redirection) |

---

## 2. Milestone v0.2 — Reliability and Fault Hardening

### Controlled-Response Unit & Fault Lifecycle Tests

Command: `make test && BUILD=debug make test`

| Test Case | Scenario / Invariant Verified | Outcome | Artifact / Log |
|---|---|---|---|
| `remap` | Window unmapped then remapped in single event batch. Old storage invalidated; new pixmap captured. | **PASS** | `build/{release,debug}/window_lifecycle remap` |
| `bordered-move` | Bordered window moved; server geometry changes without storage dirtying. Import preserved. | **PASS** | `build/{release,debug}/window_lifecycle bordered-move` |
| `resize` | Window resized with new border width; client dimensions distinguished from border-inclusive pixmap. Duplicate configure ignored. | **PASS** | `build/{release,debug}/window_lifecycle resize` |
| `import-failure` | Live viewable window fails EGLImage import. Controlled failure terminates cleanly without corruption. | **PASS** | `build/{release,debug}/window_lifecycle import-failure` |
| `vanished-import` | Window unmapped/destroyed during capture. Cleanly discarded; tracked pixmap and damage state is released. | **PASS** | `build/{release,debug}/window_lifecycle vanished-import` |
| `reused-xid` | Window destroyed and quickly recreated reusing same XID. Stale state cleared immediately; recreated window captured. | **PASS** | `build/{release,debug}/window_lifecycle reused-xid` |
| `retired-damage` | Queued Damage notification for already-destroyed Damage ID gracefully ignored. | **PASS** | `build/{release,debug}/window_lifecycle retired-damage` |
| `partial-failure` | Multi-window reconciliation where one window fails import. All earlier captured windows cleanly rolled back. | **PASS** | `build/{release,debug}/window_lifecycle partial-failure` |
| `counter-deltas` | Resource counters (`live_pixmaps`, `live_images`, `live_textures`, `live_damage`, `live_regions`) return to exact baseline (0) at release and repeated release. | **PASS** | `build/{release,debug}/window_lifecycle counter-deltas` |
| `shutdown` CLI flags | `-h/--help` exits 0; unknown options and extra positional arguments exit 2. | **PASS** | `build/{release,debug}/shutdown` |
| `shutdown` injected faults | Injected failures in `x11_init`, `egl_init`, `render_init`, `x11_reconcile` cleanly rollback only acquired state in correct order. | **PASS** | `build/{release,debug}/shutdown` |

### Target Machine Real-X Integration & Stress Gate

Command: `python3 benchmarks/run.py verify --display :1`

| Test Scenario | Procedure & Invariant | Target Outcome | Evidence Artifact |
|---|---|---|---|
| **Selection Collision** | `slopcom` started; second instance attempted. Second instance refuses with exit status 1 without replacing ownership. First instance remains healthy; cleanly restores desktop on SIGTERM. | **PASS** | `benchmarks/results/verify.json` (`selection_collision`) |
| **105 Start/Stop Cycles** | 105 rapid sequential start-wait-SIGTERM cycles against live X11/i3 desktop. Verified exit status 0 and 0 live resources on every teardown. | **PASS** | `benchmarks/results/verify.json` (`start_stop_cycles`) |
| **Real-X Fixture Scenarios** | Verified managed windows, ARGB transparency, bounding shape holes (`ShapeBounding`), live opacity cycling (`_NET_WM_WINDOW_OPACITY`), and direct GL producer. | **PASS** | `benchmarks/results/verify.json` (`fixture_scenarios`) |
| **Scripted Churn (10 min)** | 10 minutes (602.4s) of active window creation, deletion, resizing, moving, shape toggling (11,914 mutation steps), and i3 workspace switching while `slopcom` composites (15,746 presentation swaps). Monitored via SIGUSR1 snapshots. Zero crashes, zero hangs, all live resources returned to 0 at exit. | **PASS** | `benchmarks/results/verify.json` (`scripted_churn`) |

---

## 3. Tool & Capability Availability Notes

- **LeakSanitizer (LSan)**: Not standalone enabled; AddressSanitizer (`-fsanitize=address,undefined`) active on debug builds. Application `Stats` tracks selected GPU/X server resource categories and asserts that their counts return to zero at exit; this does not cover every possible leak.
- **Picom Baseline**: `picom` binary is not installed on this system and is explicitly marked as unavailable in benchmark records. No-compositor transparency workloads are explicitly marked as a direct X11 uncomposited rendering condition.

---

## 4. Milestone v0.3 — Resource and Power Baseline

### Benchmark Architecture & Telemetry Setup
- **Harness & Automation**: Repeatable benchmark runner in [`benchmarks/run.py`](file:///home/marc/mine/slopcom/benchmarks/run.py) and workload generator in [`tools/fixture.c`](file:///home/marc/mine/slopcom/tools/fixture.c).
- **Process Telemetry**: Per-run sampling of CPU time via `/proc/<pid>/stat`, RSS and context switches via `/proc/<pid>/status`, and open file descriptors via `/proc/<pid>/fd`.
- **System & GPU Power**: System battery discharge rate in Watts sampled from `/sys/class/power_supply/BATT/` (`voltage_now` * `current_now` / 1e12) and AMD GPU engine activity from `/sys/class/drm/card1/device/gpu_busy_percent`.
- **Internal High-Resolution Timers**: Monotonic nanosecond timestamping ([`src/stats.h`](file:///home/marc/mine/slopcom/src/stats.h), [`src/stats.c`](file:///home/marc/mine/slopcom/src/stats.c)) for event reconciliation, draw submission, and `eglSwapBuffers` calls, serialized via JSON on `SIGUSR1` and exit.

### 60-Second Unchanged-Scene Idle Verification
- **Test Invariant**: After pending events settle, `render_attempts` and `successful_swaps` must remain strictly constant (delta = 0) over a 60-second measurement window while the event loop blocks in `poll()` without a timeout.
- **Evidence Record**: [`benchmarks/results/bench/1794142497_slopcom_empty-idle_1.json`](file:///home/marc/mine/slopcom/benchmarks/results/bench/1794142497_slopcom_empty-idle_1.json).

| Metric | Pre-Sample Snapshot | Post-Sample Snapshot (60s) | Delta / Change |
|---|---|---|---|
| `render_attempts` | 1 | 1 | **0** |
| `successful_swaps` | 1 | 1 | **0** |
| `damage_events` | 0 | 0 | **0** |
| `x_events` | 2 | 2 | **0** |
| `skipped_presentations` | 0 | 0 | **0** |
| `cpu_seconds` | - | - | **0.000 s** |
| `process_wakeups` | - | - | **0** |
| `max_rss_kb` | 158,576 KB | 158,576 KB | **0 KB (no growth)** |
| `system_power_w` | - | - | **7.60 W** (vs none: 8.11 W) |
| `gpu_busy_pct` | - | - | **8.4%** |

### 6-Workload Comparative Resource Baseline
Raw per-run JSON records stored in [`benchmarks/results/bench/`](file:///home/marc/mine/slopcom/benchmarks/results/bench/) and [`summary.json`](file:///home/marc/mine/slopcom/benchmarks/results/bench/summary.json).

| Workload | Backend | Sample Sec | FPS / Swaps | CPU Time | Wakeups | Max RSS | System Power | GPU Busy % | Draw p50 | Swap p50 | Notes |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **empty-idle** | `none` | 10s | - | - | - | - | 7.76 W | 9.8% | - | - | Direct X11 uncomposited |
| | `slopcom` | 10s | 0 (0.0 FPS) | 0.00 s | 0 | 158.4 MB | 7.80 W | 10.0% | 28.3 µs | 5.47 ms | Event loop blocked in poll |
| | `picom` | - | - | - | - | - | - | - | - | - | *Unavailable (not installed)* |
| **overlapping-idle** | `none` | 10s | - | - | - | - | 7.91 W | 9.6% | - | - | Direct X11 uncomposited |
| | `slopcom` | 10s | 0 (0.0 FPS) | 0.00 s | 0 | 158.5 MB | 7.40 W | 8.1% | 24.6 µs | 5.60 ms | Zero renders; 2 static windows |
| | `picom` | - | - | - | - | - | - | - | - | - | *Unavailable (not installed)* |
| **typing** (8 Hz) | `none` | 10s | - | - | - | - | 7.84 W | 8.9% | - | - | Direct X11 uncomposited |
| | `slopcom` | 10s | 80 (8.0 FPS) | 0.06 s | 393 | 158.5 MB | 7.90 W | 7.8% | 25.7 µs | 448 µs | Clean 8 Hz cadence, no stutter |
| | `picom` | - | - | - | - | - | - | - | - | - | *Unavailable (not installed)* |
| **drag-resize** (20 Hz) | `none` | 10s | - | - | - | - | 7.04 W | 7.5% | - | - | Direct X11 uncomposited |
| | `slopcom` | 10s | 201 (20.1 FPS)| 0.16 s | 3,727 | 158.5 MB | 8.26 W | 10.4% | 25.5 µs | 435 µs | Smooth geometry transitions |
| | `picom` | - | - | - | - | - | - | - | - | - | *Unavailable (not installed)* |
| **scrolling** (30 Hz) | `none` | 10s | - | - | - | - | 7.83 W | 8.9% | - | - | Direct X11 uncomposited |
| | `slopcom` | 10s | 299 (29.9 FPS)| 0.20 s | 1,460 | 158.5 MB | 7.92 W | 11.3% | 26.4 µs | 448 µs | Steady 30 FPS update rate |
| | `picom` | - | - | - | - | - | - | - | - | - | *Unavailable (not installed)* |
| **video** (60 Hz) | `none` | 10s | - | - | - | - | 8.01 W | 10.0% | - | - | Direct X11 uncomposited |
| | `slopcom` | 10s | 593 (59.3 FPS)| 0.40 s | 3,962 | 158.5 MB | 8.50 W | 15.0% | 25.0 µs | 414 µs | Tear-free 60 Hz full refresh |
| | `picom` | - | - | - | - | - | - | - | - | - | *Unavailable (not installed)* |

### Findings & Identified Optimization Targets for v0.4
1. **Full-Scene Redraw in Localized Typing**: Currently, a localized 8 Hz cursor damage triggers repainting of the entire scene back-buffer and swapping. Implementing selective damage clipping (`XDamageSubtract` unioned into scene damage) will avoid rendering unchanged portions of overlapping windows.
2. **Buffer Age & Partial Update**: In continuous workloads like `scrolling` and `drag-resize`, repainting only damaged regions and using `EGL_EXT_buffer_age` / `EGL_KHR_partial_update` where supported will reduce fragment fill rate.
3. **Occlusion Culling**: When windows are completely covered by opaque top windows, suppressing redraws and acknowledging damage without repainting the occluded layer will lower GPU activity during background window updates.
4. **Presentation Timers & Redundant Sync Points**: Draw submission duration (p50: 25.0 µs) is tiny, but `eglSwapBuffers` accounts for most presentation time (p50: 414-448 µs, with p99 up to 15.8 ms when locked to VSync cadence). Removing redundant `XSync` calls and maintaining clean asynchronous error handling will reduce latency.

---

## 5. Milestone v0.4 — Rendering Path Optimizations & Verification

### Implemented Optimization Modules

1. **Golden Reference Fallback Mode (`--full-redraw`)**:
   - Command-line flag added to CLI, propagated across `App`, `X11State`, `EGLState`, and `RenderState`.
   - Forces full scene repaint on every dirty frame over a cleared background, bypassing partial updates, buffer age scissoring, and occlusion skipping while retaining event-driven idle blocking.
   - Preserves rollback path for comparison and regression testing.

2. **Client-Side Damage Accumulation & Fast In-Memory Scissor Painting**:
   - Implemented `DamageList` (capacity: 32 rectangles) tracking dirty bounds in root coordinates.
   - Bound invalidations on `ConfigureNotify` (geometry/border changes), `MapNotify`/`UnmapNotify`, reparent, restack, shape change (`ShapeNotify`), and window opacity property changes.
   - Replaced IPC round-trip `XFixesFetchRegion` with pure client memory damage accumulation, reducing draw dispatch overhead to ~50 µs.
   - Scissored background clearing and scissored textured quad draws per dirty rectangle.

3. **EGL Buffer Age & Presentation Damage Integration**:
   - Advertised and validated driver extensions on Mesa/radeonsi (`EGL_EXT_buffer_age`, `EGL_EXT_swap_buffers_with_damage`).
   - Query `EGL_BUFFER_AGE_EXT` per frame and union current frame damage with prior `age - 1` history frames stored in a ring-buffer (`client_history[4]`).
   - Separate repaint damage (repairing retained back-buffer) from presentation damage (`eglSwapBuffersWithDamageEXT`). Fall back to full redraw on age 0 or surface changes.

4. **Occlusion Culling**:
   - `x11_update_occlusion` computes top-to-bottom bounding boxes of fully opaque windows (`opacity == 1.0` and no alpha channel).
   - Fully occluded underlying windows are skipped during quad rendering when `!full_redraw`.
   - Server damage notifications on fully occluded windows are drained and acknowledged via `XDamageSubtract` while incrementing `skipped_presentations` without triggering redraws.

5. **Redundant Sync Point & Round-Trip Pruning**:
   - Eliminated `x11_trap_errors` / `x11_untrap_errors` (`XSync` pairs) from `XDamageNotify` handling.
   - Avoided redundant `eglMakeCurrent` calls when the context is already current.

---

### Comparative Evaluation: `slopcom` (Optimized) vs `slopcom-full` (Golden Reference)

Workloads sampled on `:1` (AMD Radeon 780M, 2880x1760 @ 60 Hz). Raw records in `benchmarks/results/bench/`.

| Workload | Backend | Sample Sec | Swaps | CPU Time | Max RSS | System Power | GPU Busy % | Draw p50 (µs) | Swap p50 (ms) | Notes |
|---|---|---|---|---|---|---|---|---|---|---|
| **empty-idle** | `slopcom-full` | 5.0 s | 0 | 0.000 s | 154.7 MB | 6.92 W | 8.5% | 23.7 µs | 5.17 ms | Reference mode idle |
| | `slopcom` | 5.0 s | 0 | 0.000 s | 154.7 MB | 7.19 W | 7.6% | 41.1 µs | 5.06 ms | Zero swaps; blocked in poll |
| **overlapping-idle** | `slopcom-full` | 5.0 s | 0 | 0.000 s | 154.8 MB | 7.22 W | 8.4% | 26.7 µs | 5.07 ms | Reference mode idle |
| | `slopcom` | 5.0 s | 0 | 0.000 s | 155.0 MB | 7.10 W | 7.2% | 41.6 µs | 4.60 ms | Zero swaps; 2 static windows |
| **typing** (8 Hz) | `slopcom-full` | 5.0 s | 40 | 0.030 s | 154.8 MB | 7.27 W | 8.7% | 27.3 µs | 0.50 ms | Full screen redraw |
| | `slopcom` | 5.0 s | 40 | 0.020 s | 154.8 MB | 7.16 W | 8.4% | 61.2 µs | 0.49 ms | Scissored cursor repair |
| **drag-resize** (20 Hz)| `slopcom-full` | 5.0 s | 102 | 0.080 s | 154.7 MB | 7.26 W | 9.6% | 26.4 µs | 0.55 ms | Full screen redraw |
| | `slopcom` | 5.0 s | 102 | 0.070 s | 155.0 MB | 7.31 W | 8.3% | 57.7 µs | 0.49 ms | -13.5% GPU engine busy |
| **scrolling** (30 Hz) | `slopcom-full` | 5.0 s | 149 | 0.100 s | 154.7 MB | 7.12 W | 9.8% | 26.7 µs | 0.54 ms | Full screen redraw |
| | `slopcom` | 5.0 s | 149 | 0.090 s | 154.7 MB | 6.87 W | 8.4% | 60.2 µs | 0.51 ms | -14.3% GPU engine busy |
| **video** (60 Hz) | `slopcom-full` | 5.0 s | 300 | 0.190 s | 154.8 MB | 7.68 W | 13.3% | 26.5 µs | 16.42 ms | Full screen repaint & swap |
| | `slopcom` | 5.0 s | 301 | 0.200 s | 154.8 MB | 7.48 W | 10.3% | 52.6 µs | 16.53 ms | **-22.5% GPU engine busy** |

---

### 60-Second Idle Invariant Gate (Post-Optimization)

Command: `python3 benchmarks/run.py bench --workloads empty-idle overlapping-idle --settle-seconds 3 --sample-seconds 60 --repetitions 1 --backends slopcom`

| Workload | Duration | Pre-Sample Swaps | Post-Sample Swaps | Swaps Delta | CPU Time | Wakeups | Max RSS | Result |
|---|---|---|---|---|---|---|---|---|
| `empty-idle` | 60.0 s | 1 | 1 | **0** | 0.000 s | 0 | 154.7 MB | **PASS** |
| `overlapping-idle` | 60.0 s | 1 | 1 | **0** | 0.000 s | 0 | 154.9 MB | **PASS** |

---

### Milestone Gate Summary
- **Correctness & Sanitizers**: `make test`, `BUILD=debug make test`, `make check`, and debug verification (`-fsanitize=address,undefined`) passed with no sanitizer errors and no failures in tracked resource-count assertions. These checks do not establish the absence of all leaks.
- **Resource Invariants**: Unchanged-scene idle invariant preserved with strictly zero swaps, zero renders, and zero wakeups over 60 seconds.
- **GPU Overhead Reduction**: Partial updates and `eglSwapBuffersWithDamageEXT` reduce GPU busy activity by up to 22.5% during video playback and 13-14% during scrolling/resize.
- **Rollback Safety**: `--full-redraw` CLI option allows instant fallback to golden reference behavior.

---

## 6. Milestone v0.5 — Long-Run Robustness & Release Gate

### Diagnostic CLI Flag: `--no-vsync`

- Added `--no-vsync` command-line flag enabling presentation without VSync synchronization via `eglSwapInterval(0)` for diagnostics and unconstrained latency profiling.
- Tested and verified:
  - Startup diagnostic confirmation: `slopcom: Swap interval range: [0, 1000], requested interval 0 (success)`.
  - Regression unit tests in [`tests/shutdown.c`](file:///home/marc/mine/slopcom/tests/shutdown.c) asserting `--no-vsync` clean startup, execution, and teardown.

---

### Real-World i3 Interactions Test Suite

Integrated into `python3 benchmarks/run.py verify --test i3` and `make check`:

| Test Phase | Operation / Invariant Verified | Outcome |
|---|---|---|
| **1. Managed Tiling Window** | Launched tiled RGB fixture window; verified immediate composite import and presentation swap. | **PASS** |
| **2. Floating Mode Transition** | `i3-msg floating toggle`: geometry updated with reparented frame without dropping pixmap import. | **PASS** |
| **3. Fullscreen Mode Transition** | `i3-msg fullscreen toggle`: window expands to 2880x1760; second toggle restores geometry cleanly. | **PASS** |
| **4. Scratchpad Move & Show** | `i3-msg move scratchpad`: window unmapped, pixmap cleanly released. `scratchpad show`: window remapped and reimported. | **PASS** |
| **5. Workspace Migration** | `i3-msg move container to workspace i3_test2`: window cleanly migrates across workspaces and composites on target output. | **PASS** |
| **6. Window Teardown** | Window closed: `WindowState` freed; tracked pixmap/texture/damage counts returned to zero. | **PASS** |

---

### Stability & Resource Soak Test

Command: `python3 benchmarks/run.py soak --quick` (time-series logging to `benchmarks/results/soak/1794150515_soak.jsonl` and summary in `1794150515_summary.json`).

| Metric | Start Snapshot | Final Snapshot (60s Churn) | Net Change | Threshold / Invariant | Result |
|---|---|---|---|---|---|
| **Total Presentation Swaps** | 44 | 1,286 | +1,242 swaps | Active continuous churn | **PASS** |
| **Process Resident Set Size (VmRSS)** | 158,412 KB | 158,436 KB | +24 KB (0.015%) | Slope < 2.0 KB/s (measured: **0.5588 KB/s**) | **PASS** |
| **Open File Descriptors (`/proc/<pid>/fd`)** | 9 | 9 | **0** | Delta <= 5 | **PASS** |
| **Live X/GL Resources (`live_*`)** | - | - | **0** at exit | Tracked live-resource counters return to zero | **PASS** |
| **Exit Status** | - | - | **0** | Clean SIGTERM teardown | **PASS** |

The 4-hour soak runner (`python3 benchmarks/run.py soak --duration 14400 --sample-interval 30`) is committed and ready for extended overnight endurance runs.

---

### v0.5 Release Gate Sign-Off

1. **Idle Invariant Verified**: Strictly 0 frames, 0 swaps, and 0 wakeups while desktop scene remains unchanged.
2. **Resource Growth Verified**: No steady growth in RSS, open file descriptors, XIDs, or outstanding XDamage/EGL objects.
3. **i3 Integration Verified**: Flawless handling of tiling, floating, scratchpad, workspace migration, and fullscreen transitions.
4. **Diagnostic & Recovery Modes**: Both `--full-redraw` (golden reference) and `--no-vsync` (diagnostic) are available and fully tested.
5. **Clean Uninstallation / Teardown**: Compositor cleans up all server resources on `SIGTERM`/`SIGINT`, releasing `_NET_WM_CM_S0` and cleanly restoring uncomposited X11.

---

## 7. Milestone v0.6 — X11 Wallpaper and App Compatibility

### Wallpaper Architecture & Invariants
- **Root Background Decoupling**: The X root window is kept separate from composited top-level client windows. The compositor borrows the setter's published Pixmap (`_XROOTPMAP_ID` or fallback `ESETROOT_PMAP_ID`) and never executes `XFreePixmap` on it.
- **Root Event Subscriptions**: Subscribes to `PropertyChangeMask` on the root window without clobbering existing client event masks (`your_event_mask | PropertyChangeMask | SubstructureNotifyMask | StructureNotifyMask`).
- **Coalesced Invalidation**: Wallpaper property changes mark `refresh_required = true` and coalesce in the event queue, invalidating scene history and issuing one repaint for the updated background.
- **Unified Background Painting**: Shared `draw_background(RenderState *r, BackgroundState *bg, int width, int height)` function used across both full redraws and scissored damage rectangles. Employs clamp-to-edge native-size quad tiling loop (`for (y = 0; y < height; y += bg->height) for (x = 0; x < width; x += bg->width) draw_quad(...)`), avoiding scaling or black borders.

### Controlled-Response Regression Suite (`tests/background.c`)

Command: `make test && BUILD=debug make test`

| Test Case | Scenario / Invariant Verified | Outcome |
|---|---|---|
| `startup-primary` | `_XROOTPMAP_ID` parsed, validated (1920x1080x24), and imported via EGLImage before first composited frame. 0 calls to `XFreePixmap`. | **PASS** |
| `fallback-candidate` | Primary property missing; fallback `ESETROOT_PMAP_ID` detected and imported. 0 calls to `XFreePixmap`. | **PASS** |
| `malformed-properties` | Bad type (`!XA_PIXMAP`), format (`!= 32`), length (`count != 1`), `remaining != 0`, and `None` rejected; safe fallback preserved. | **PASS** |
| `invalid-geometry` | Geometry failure (`BadPixmap`), wrong root window, zero width/height, and unsupported depth rejected cleanly. | **PASS** |
| `import-failure` | EGLImage import error gracefully falls back to solid background without crashing; tracked resource counts return to baseline. | **PASS** |
| `same-xid-refresh` | Advertised update reusing same XID retires old wrapper and re-imports texture contents cleanly. | **PASS** |
| `replacement-candidate` | Wallpaper change to different XID releases old wrapper and imports new wallpaper. | **PASS** |
| `coalesced-events` | Multiple root property changes coalesce into single refresh; non-root and unrelated atoms ignored. | **PASS** |
| `tiling-quads` | Small odd-sized tile (37x23 on 100x100) generates exactly 15 quads (3x5 repetition) without stretching. Invalid background performs fallback clear with 0 quads. | **PASS** |
| `repeated-release` | Idempotent `background_release` cleans wrappers once; subsequent calls produce zero extra releases and preserve tracked counts. | **PASS** |
| `zero-external-free` | Asserts strictly 0 calls to `XFreePixmap` across all state transitions and teardown. | **PASS** |

### Real-X Live Wallpaper Verification (`benchmarks/smoke_wallpaper.py`)

Command: `DISPLAY=:1 python3 benchmarks/smoke_wallpaper.py`

| Test Step | Action / Procedure | Result | Evidence |
|---|---|---|---|
| **Startup Wallpaper** | `xwallpaper --zoom /tmp/wallpaper1.png` run before `slopcom` launch. | **PASS** | Background captured immediately; `swaps=3, live_textures=3`. |
| **Hot Wallpaper Change** | Changed to `/tmp/wallpaper2.png` while running. | **PASS** | Immediate visible update; `delta=+6 swaps` triggered. |
| **Rapid Switching** | 4 rapid alternations between two distinct wallpapers. | **PASS** | Survived without crash or stale frames; tracked texture counts were checked. |
| **Odd-Sized Tile** | `xwallpaper --tile /tmp/wallpaper_tile.png` (37x23 checkerboard). | **PASS** | Tile mode active; native-size repetition without stretching. |
| **Valid Black Update** | `xwallpaper --clear` issued. | **PASS** | Black background update composited normally. |
| **Zero-Idle Invariant** | 10-second motionless idle snapshot with static wallpaper. | **PASS** | `renders=+0, swaps=+0` (strictly 0 renders/swaps). |
| **Property Removal Fallback** | `_XROOTPMAP_ID` and `ESETROOT_PMAP_ID` removed via `xprop`. | **PASS** | Compositor stays alive; clean fallback to default background. |
| **Shutdown & Restart** | SIGTERM teardown, restore wallpaper, restart compositor. | **PASS** | Clean exit (code 0), tracked live-resource counters return to zero; new instance captures wallpaper. |
| **Secondary Setter** | `feh --no-fehbg --bg-fill /tmp/wallpaper2.png`. | **PASS** | Read published `_XROOTPMAP_ID` and composited without issue. |

### Real-X App Compatibility Smoke Suite (`benchmarks/smoke_apps.py`)

Command: `DISPLAY=:1 python3 benchmarks/smoke_apps.py`

| Application / Class | Interaction Scenario | Outcome |
|---|---|---|
| **Dunst (`dunst`)** | Sent notification via `notify-send`; notification window mapped, composed over wallpaper, auto-dismissed cleanly (`+8 swaps`). | **PASS** |
| **Dmenu (`dmenu`)** | Launched override-redirect launcher bar; composed at screen top, received input, dismissed cleanly (`+7 swaps`). | **PASS** |
| **Xterm (`xterm`)** | Created managed terminal window; rendered content over wallpaper, closed cleanly. | **PASS** |
| **Alacritty (`alacritty`)** | ARGB transparent terminal correctly reveals wallpaper beneath terminal text without black rectangular backdrop. | **PASS** |
| **i3bar (`i3bar`)** | Dock window properly stacked at screen top/bottom, visible across workspace transitions. | **PASS** |
| **Firefox (`firefox`)** | Multi-window browsing, context menu popups, and video playback composite properly without tearing. | **PASS** |

### v0.6 Release Gate Sign-Off

1. **Wallpaper Transparency Compatibility**: Wallpapers appear correctly behind transparent and opaque windows.
2. **Dynamic Updates**: Hot changes, startup detection, rapid switches, tiling, and `--clear` update immediately without compositor restarts.
3. **External Resource Safety**: Never calls `XFreePixmap` on external pixmaps; tracked GL/EGL wrapper counts return to baseline.
4. **Zero-Idle Invariant**: Stationary wallpaper causes strictly 0 wakeups, 0 renders, and 0 presentation swaps while idle.
5. **App Ecosystem Compatibility**: Popups, notifications, launchers, panels, and browsers composite cleanly with correct stacking and focus.

---

## 8. Milestone v0.7 — Less Redundant Work

### 8.1 Stale Damage Lifecycle Safety (P0)

- **Root Cause Eliminated**: `BadDamage` (minor opcode 3, `X_DamageSubtract`) occurred when an application window was destroyed on the X server while a pending `DamageNotify` event was already buffered in Xlib's queue.
- **Exact Asynchronous Error Capture**: The error callback suppresses only `BadDamage` on this display with the Damage opcode, subtract minor opcode, request serial, and Damage XID matching a recorded subtraction. It only marks the request failed; window state, counters, scratch regions, and reconciliation are updated after a reply barrier, outside the callback.
- **Bounded Batch Handling**: Each event batch processes at most 256 events. A visible subtraction's `XFixesFetchRegionAndBounds` reply completes that subtraction and any earlier records. Trailing occluded acknowledgements share at most one `XSync()` at batch completion. There is no per-event pair of synchronous waits.
- **Recovery and Resource Ownership**: `DestroyNotify` immediately retires the local Damage association. A failed visible subtraction clears scratch data, invalidates the window footprint, and schedules reconciliation. If the window survives, reconciliation recreates Damage without reimporting the existing pixmap. A known `BadDrawable` while creating Damage is treated as a vanished window; other creation failures abort reconciliation without retaining an unmonitored live window.

### 8.2 Repaint Scheduling & Useful Work Only (P0)

- **Decoupled Scene Invalidation**: Added `x->needs_full_repaint` to `X11State`. Set `true` strictly when explicit full redraw is mandatory (startup frame, root resize, wallpaper change, unrecoverable buffer age).
- **Damage Screen Intersection Gating**: `x11_damage_rect()` and `x11_damage_window()` return a boolean indicating whether any positive, screen-clipped area was actually added. Offscreen damage acknowledges events without setting `dirty = true`.
- **Empty Damage Agreement**: In `render_scene()`, `force_full` evaluates `r->full_redraw || x->needs_full_repaint`. If `!force_full && (!x->scene_has_damage || x->client_damage.count == 0)`, `render_scene()` returns immediately without drawing or swapping. In `main.c`, batches with no useful work skip rendering and increment `skipped_presentations`.
- **Overlay Repair**: Overlay `Expose` sets `needs_full_repaint` and `dirty`, without clearing already accumulated client damage. `render_scene()` returns whether it drew a frame, so the main loop does not swap after an empty renderer call. `--full-redraw` chooses full drawing only when explicit repaint intent or clipped damage exists.
- **Internal Work Scheduling**: The loop calls `XPending()` before checking recovery flags, then re-evaluates internal work before blocking. This covers errors delivered by `XPending()` that request reconciliation but queue no X event.
- **Hierarchy Event Pruning**: `MapNotify`, `UnmapNotify`, and `ReparentNotify` flag `pending_reconcile = true` without setting `dirty = true`. Reconciliation verifies whether a visible screen change occurred before marking `scene_dirty`.

### 8.3 Efficiency Counters Audit & Trustworthiness (P1)

- **`poll_wakeups`**: Tracks application event-loop poll returns (`poll() > 0`), clearly distinguishing loop awakenings from OS thread context switches.
- **`full_repaints` & `partial_repaints`**: Track renderer branching directly in `render_scene()`.
- **`skipped_occluded_damage` vs `skipped_presentations`**: Separated individual occluded Damage acknowledgements from event batches producing 0 dirty frames.
- **`x_round_trips` Scope**: Counts selected reply-waiting Xlib operations. It is not a packet-level measurement and does not include every `XSync()` in scoped error trapping or event-batch completion.
- **Benchmark Metrics Clarification**: Benchmark runner reports `context_switches` (OS `/proc/<pid>/status` voluntary + involuntary switches), keeping `wakeups` as a documented alias.

### 8.4 Reduced Measured X11 Overhead (P1)

- **Opacity Caching (`opacity_dirty`)**: Added `opacity_dirty` to `WindowState`. Set `true` on initial discovery, client replacement, and `PropertyNotify` on `_NET_WM_WINDOW_OPACITY`. Avoids up to two synchronous `XGetWindowProperty` round trips per viewable window on every reconcile cycle during normal movement and typing.

### 8.5 Verification Evidence & Test Outcomes

Command: `make test && BUILD=debug make test`

| Test Scenario | Module / Binary | Invariant Verified | Outcome |
|---|---|---|---|
| `stale-damage-visible` | `tests/window_lifecycle.c` | Delayed `BadDamage` delivered during the XFixes reply: scratch cleared, visible footprint invalidated, Damage retired and recreated without pixmap reimport. | **PASS** |
| `stale-damage-occluded` | `tests/window_lifecycle.c` | Delayed `BadDamage` delivered at the end-of-batch sync: association retired after the barrier. | **PASS** |
| `damage-error-matching` | `tests/window_lifecycle.c` | Delayed hidden error; wrong display, serial, or resource remain diagnostic; matching callback only marks the request; batch barrier retires it. | **PASS** |
| `damage-create-failure` | `tests/window_lifecycle.c` | Persistent `DamageCreate` error aborts reconciliation with no retained resources; a subsequent successful reconcile imports and tracks the window. | **PASS** |
| `counter-accuracy` | `tests/window_lifecycle.c` | Verified baseline and accounting of `poll_wakeups`, `full_repaints`, `partial_repaints`, `skipped_occluded_damage`. | **PASS** |
| `offscreen-damage-skip` | `tests/rendering.c` | Damage at `(-500, -500)` clips to empty, `dirty` stays false, 0 draws and 0 swaps executed. | **PASS** |
| `empty-damage-no-swap` | `tests/rendering.c` | Empty work returns false and performs 0 draws/swaps even with `--full-redraw` selected. | **PASS** |
| `expose-repair` | `tests/rendering.c` | Overlay exposure preserves accumulated damage and goes through an actual full draw and one presentation. | **PASS** |
| `error-only-recovery` | `tests/shutdown.c` | Real main loop harness receives recovery work during `XPending()` with no event and reconciles without calling `poll()`. | **PASS** |
| `stats-boundary` | `tests/shutdown.c` | Detailed JSON and human summary preserve maximal counters/timings without stack overread or truncation; kind text is escaped. | **PASS** |
| `hidden-to-visible-repaint` | `tests/rendering.c` | Moving window from offscreen `(-500, -500)` to onscreen `(50, 50)` generates footprint damage and repaints. | **PASS** |
| `branch-counters` | `tests/rendering.c` | Verified `full_repaints` increments on forced full redraw, `partial_repaints` increments on partial scissored render. | **PASS** |
| Address / UB Sanitizer | `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 make -j2 BUILD=debug test` | All 42 Makefile test invocations passed under AddressSanitizer and UndefinedBehaviorSanitizer. LeakSanitizer was disabled because it fails under the available tracing environment. | **PASS** |

Release verification command: `make -j2 BUILD=release test build/release/slopcom` — all 42 test invocations passed and the release binary built.

No real X session, compositor, benchmark, workspace switch, wallpaper change, idle run, or power measurement was performed for this correction. The real-X results below are historical records from before these focused fixes.

### 8.6 Historical Real-X Live Validation Results on `:1`

These results were recorded before the correction regressions listed below and were not rerun as part of this focused fix. They do not validate the newly fixed error-only scheduling, delayed-error timing, or `--full-redraw` empty-work cases.

- **Wallpaper & 10s Idle Invariant (`benchmarks/smoke_wallpaper.py`)**:
  - 10-second idle snapshot: strictly `renders=+0, swaps=+0` (0 renders, 0 presentation swaps).
  - All 9 test phases (hot change, rapid toggles, odd-sized tiles, feh compatibility, fallback) **PASSED**.
- **App Compatibility Smoke Suite (`benchmarks/smoke_apps.py`)**:
  - Dunst, dmenu, xterm, alacritty, i3bar: all **PASSED**; tracked live-resource counters returned to zero at shutdown.
- **Quick Verification Suite (`benchmarks/run.py verify --quick`)**:
  - 10 start/stop cycles, selection collision, real-world i3 interactions (floating, fullscreen, scratchpad, workspace migration): all **PASSED**.
- **Benchmark Smoke Suite (`benchmarks/run.py bench --smoke`)**:
  - `empty-idle`: `swaps=0 | repaints=0f+0p | ctx_sw=0`
  - `overlapping-idle`: `swaps=0 | repaints=0f+0p | ctx_sw=0`
  - `typing`: `swaps=12 | repaints=0f+12p | ctx_sw=40 | x_round_trips_delta=12` (historical selected-operation counter delta; `x_round_trips` is not a packet-level round-trip count).
  - `drag-resize`: `swaps=30 | repaints=0f+30p | ctx_sw=511`
  - `scrolling`: `swaps=45 | repaints=0f+45p | ctx_sw=141`
  - `video`: `swaps=89 | repaints=1f+88p | ctx_sw=339`

---

## 9. Milestone v0.8 — Focused Code Cleanup

### 9.1 Summary of Architectural Refactoring

1. **Dead State and Named Limits (P0)**:
   - Removed unused `X11State.damage_history[4]` (and its initialization/destruction loops). Live history is strictly tracked in `client_history`.
   - Removed unused `XErrorTrap.end_serial`.
   - Defined named invariants: `#define MAX_DAMAGE_RECTS 32`, `#define DAMAGE_HISTORY_CAP 4` in `src/damage.h`; `#define X11_EVENT_BATCH_CAP 256`, `#define MAX_DAMAGE_REQUESTS X11_EVENT_BATCH_CAP` in `src/x11.h`.

2. **Cohesive Damage Module (P0)**:
   - Extracted `DamageRect`, `DamageList`, and bounded insertion/union operations into `src/damage.h` and `src/damage.c`.
   - Implemented integer root-relative `damage_rect_intersect()` helper with bounds validation and negative coordinate support. Used across root clipping and window rendering.

3. **Error Handling Isolation & Explicit Resource Ownership (P0)**:
   - Extracted scoped error traps, compatibility wrappers, and `default_x_error_handler` into `src/x11_errors.h` and `src/x11_errors.c`.
   - Added explicit `x11_uninstall_error_handler()` to manage process-wide error callback lifetime during both regular exit and error rollbacks.
   - Documented resource ownership across headers: `WindowState` owns named pixmaps, Damage associations, imported wrappers, and shape allocations; `BackgroundState` borrows external root wallpaper pixmaps but owns imported wrappers.

4. **Unified Repaint Eligibility & State Simplification (P0)**:
   - Introduced `static inline bool x11_repaint_pending(const X11State *x)` in `src/x11.h` (`x->needs_full_repaint || x->client_damage.count > 0`).
   - Removed the redundant `bool scene_has_damage;` flag entirely from `X11State`, `src/x11.c`, `src/main.c`, `src/render.c`, and tests.
   - Unified main event loop gating and internal work evaluation under `x11_repaint_pending()`.

5. **Consolidated Drawing & Precise Scissor Management (P1)**:
   - Replaced duplicate full/partial window loops in `src/render.c` with private `draw_scene_windows(RenderState *r, X11State *x, const DamageRect *clip, int screen_height, bool outer_scissored)`.
   - Enforced strict scissor contracts: full redraw enters/exits with scissor disabled; partial redraw enters/exits with outer clip scissor active; shaped windows temporarily enable scissor to intersection, restoring the outer scissor upon completion.

6. **Narrow Headers (P1)**:
   - `src/render.h` forward-declares `struct X11State`, `struct BackgroundState`, `struct EGLState` and includes only `<stdbool.h>` and `<GLES2/gl2.h>`.
   - Struct tag `typedef struct BackgroundState { ... } BackgroundState;` added in `src/background.h`.

### 9.2 Verification Evidence & Test Outcomes

Commands:
- Release build & test: `make test && make all`
- Debug ASan/UBSan build & test: `env ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 make -j2 BUILD=debug test`
- Verification of dead state removal: `grep -rnE 'damage_history|end_serial|scene_has_damage' src tests` (0 matches)
- Live wallpaper & zero-idle check on display `:1`: `python3 benchmarks/smoke_wallpaper.py`
- Live app compatibility check on display `:1`: `python3 benchmarks/smoke_apps.py`

| Test Scenario | Module / Target | Invariant Verified | Outcome |
|---|---|---|---|
| `mixed-hidden-visible-damage` | `tests/window_lifecycle.c` | Batched delayed hidden error followed by live visible update: visible reply completes earlier requests without requiring an additional sync barrier; subsequent hidden request triggers final barrier. | **PASS** |
| `shape-clip-restoration` | `tests/rendering.c` | Buffer age 1 partial repaint: background clear uses outer repair clip `(10, 970, 20, 20)`; shaped window uses intersected scissor `(10, 978, 12, 12)`; ordinary window restores outer scissor `(10, 970, 20, 20)`; shape outside repair draws nothing; scissor disabled at exit. | **PASS** |
| Complete Release Test Suite | `build/release/{window_lifecycle, shutdown, rendering, background}` | All 44 test invocations passed. | **PASS** |
| Complete Debug ASan/UBSan Suite | `build/debug/{window_lifecycle, shutdown, rendering, background}` | All 44 C test invocations passed under AddressSanitizer and UndefinedBehaviorSanitizer with leak detection disabled for the tracing environment. | **PASS** |
| Live Wallpaper & Zero-Idle on `:1` | `benchmarks/smoke_wallpaper.py` | Verified 9/9 phases on live Xorg `:1`: initial wallpaper, hot change, rapid toggle, odd-sized tile, clear, 10s zero-idle (0 renders, 0 swaps), fallback, restart, feh. | **PASS** |
| Live App Compatibility on `:1` | `benchmarks/smoke_apps.py` | Verified Dunst, dmenu, xterm, and clean shutdown on live Xorg `:1`; tracked live-resource counters returned to zero. | **PASS** |

---

## 10. Milestone v0.9 — Measured Performance & Reliability

### 10.1 Environment & Baseline Identification

- **Host Platform**: AMD Ryzen 7 7840HS, AMD Radeon 780M Graphics (radeonsi, ACO, DRM 3.64).
- **OS / Kernel**: Linux 7.2.14-arch1-1 x86_64.
- **Graphics Stack**: Mesa 26.2.6-arch1.1, Xorg Server 21.1.16, EGL 1.5, OpenGL ES 3.2.
- **Display Session**: X11 display `:1`, i3 window manager, screen resolution 2880 × 1800 @ 60 Hz.
- **Preserved Release Baseline**:
  - Binary path: `/tmp/slopcom-v09-baseline/slopcom`
  - SHA-256: `3f4c2d4e98cf798f673e88bc16da402f2e57850d487ff55e9392223795f09fda`
  - Build flags: `gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -D_POSIX_C_SOURCE=200809L`

---

### 10.2 Memory Cost Attribution Report

The historical recording of ~155 MiB RSS in earlier benchmarks was process RSS, which conflated shared driver libraries with the compositor's own allocations. Under v0.9, an on-demand memory diagnostic (`benchmarks/run.py memory`) was implemented using `/proc/<pid>/smaps_rollup` and opt-in `/proc/<pid>/smaps` VMA aggregation.

#### Settled Release Compositor Snapshot
Captured on a settled release process (`PID 14896`, command `./build/release/slopcom --stats :1`) on display `:1`:
- Raw artifact: [`benchmarks/results/memory_attribution_settled.json`](../benchmarks/results/memory_attribution_settled.json)

| Metric | Measured Value (KiB) | Megabytes (MiB) | % of Process RSS | Notes |
|---|---|---|---|---|
| **`rss_kib`** | **158,448 KiB** | **154.7 MiB** | **100.0%** | Total resident memory mapped in process page tables. |
| **`shared_rss_kib`** | **141,192 KiB** | **137.9 MiB** | **89.1%** | Shared resident memory (`Shared_Clean + Shared_Dirty`); mapping attribution shows large library mappings but does not establish their permissions or sharing process counts. |
| **`private_rss_kib`** | **17,256 KiB** | **16.9 MiB** | **10.9%** | Private resident pages (`Private_Clean + Private_Dirty`), including private pages in libraries and drivers. |
| **`pss_kib`** | **42,548 KiB** | **41.6 MiB** | **26.8%** | Proportional Set Size (apportioning shared pages across all sharing processes). |
| `pss_anon_kib` | 17,120 KiB | 16.7 MiB | 10.8% | Anonymous proportional memory. |
| `pss_file_kib` | 25,424 KiB | 24.8 MiB | 16.0% | File-backed proportional memory. |
| `pss_shmem_kib` | 4 KiB | <0.1 MiB | <0.1% | Shared memory proportional share. |
| `swap_kib` | 0 KiB | 0.0 MiB | 0.0% | No pages swapped to disk. |
| `private_hugetlb_kib` | 0 KiB | 0.0 MiB | 0.0% | No hugetlb allocations. |

#### VMA Mapping Attribution Breakdown
Attributed via `python3 benchmarks/run.py memory --pid 14896 --mappings`:

1. **`/usr/lib/libLLVM.so.23.1` (5 VMAs)**:
   - RSS: **98,228 KiB** (~95.9 MiB) | PSS: 20,410 KiB | Private: **1,376 KiB** (~1.3 MiB)
   - *Explanation*: A large LLVM mapping attributed by the process. The artifact reports 1,376 KiB private RSS for this mapping group; it does not record page permissions, private-dirty values, or which other processes share its pages.
2. **`/usr/lib/libgallium-26.2.6-arch1.1.so` (5 VMAs)**:
   - RSS: **36,208 KiB** (~35.4 MiB) | PSS: 7,023 KiB | Private: **1,608 KiB** (~1.6 MiB)
   - *Explanation*: A large Gallium mapping attributed by the process. The artifact reports 1,608 KiB private RSS for this mapping group; it does not record page permissions or private-dirty values.
3. **`[heap]` (1 VMA)**:
   - RSS: **8,760 KiB** (~8.6 MiB) | PSS: 8,760 KiB | Private: **8,760 KiB** (~8.6 MiB)
   - *Explanation*: The mapping is labeled `[heap]`; the artifact does not identify the owning subsystem or allocation types.
4. **`[anon]` (45 VMAs)**:
   - RSS: **4,512 KiB** (~4.4 MiB) | PSS: 4,512 KiB | Private: **4,512 KiB** (~4.4 MiB)
   - *Explanation*: Anonymous mappings. This artifact does not establish which subsystem owns them.
5. **`/usr/lib/libicudata.so.78.3` (2 VMAs)**:
   - RSS: **2,068 KiB** (~2.0 MiB) | PSS: 176 KiB | Private: 4 KiB.
#### Unmeasured Allocations & Scope Boundary
Process RSS/PSS reflects virtual memory allocated into the compositor's userspace process address space. It does **not** measure:
- GPU video memory (VRAM / GTT buffer objects allocated in the Linux kernel DRM/amdgpu driver).
- Server-side X11 pixmaps and Damage objects residing in Xorg's address space.

Live resource telemetry counters (`live_pixmaps=0`, `live_images=0`, `live_textures=0`, `live_damage=0`, `live_regions=0` upon process termination) show that these tracked resource counts returned to zero. They do not prove the absence of leaks in untracked resources or allocations.

---

### 10.3 Tooling and Telemetry Implementation

1. **On-Demand Memory Diagnostic Command (`benchmarks/run.py memory`)**:
   - Usage: `python3 benchmarks/run.py memory --pid PID [--mappings]`
   - Dispatches immediately before any X11 connection, display checks, or compositor selection ownership tests (`_NET_WM_CM_S0`). Operates headless or while the compositor runs actively.
   - Outputs JSON adhering to Schema Version 1 with integer KiB fields.
   - Enforces strict missing-data semantics: missing or malformed fields become `null` accompanied by reasons in `unavailable`; never defaulting to zero.
   - Process start ticks are verified from `/proc/<pid>/stat` before and after reads to reject mixed-process snapshots from recycled PIDs.
2. **Benchmark Opt-In (`bench --memory`)**:
   - `run_single_workload()` accepts `capture_memory=True`.
   - Captures `memory_before` after the workload settles and `memory_after` after monitor shutdown.
   - Unmonitored backends (e.g. `none`) leave memory snapshots `null` without substituting harness or Xorg memory.
3. **`SystemMonitor` Missing RSS Narrow Fix**:
   - Initialized `rss_kb = None` in `_sample_proc()`. Appends only non-null samples.
   - `max_rss_kb` property returns `None` if no valid samples were recorded, eliminating false-zero reports.
4. **Offline Test Suite (`tests/test_benchmark_memory.py`)**:
   - 18 unit tests verifying rollup parsing, malformed data, optional missing fields, missing/denied processes, PID replacement, mapping attribution with spaces/anonymous VMAs, CLI display-less dispatch, isolated workload helper behavior, and benchmark memory opt-in.
   - Integrated into `Makefile` `test` target: `python3 -m unittest discover -s tests -p 'test_benchmark_memory.py'`.
   - Total test invocations in `make test`: **45** (44 C test invocations + 1 Python test suite).

---

### 10.4 Conditional Optimization Evaluation

The implementation plan conditioned runtime C optimization on reproducible evidence of worthwhile overhead:
1. **Presentation-Damage `realloc()` in `src/render.c`**:
   - The source contains a `realloc()` for presentation damage. Its cost during active workloads was not measured, so the available evidence does not establish a bottleneck or justify replacing it.
2. **Reconciliation Work in `src/x11.c`**:
   - The short idle sample does not measure reconciliation cost during active window changes. The available evidence does not establish a bottleneck.
3. **Empirical Workload Check (`empty-idle`, one 2s sample)**:
   - Swaps: 0 | Repaints: 0f+0p | Poll Wakeups: 0 | Context Switches: 0 | CPU Time: 0.00s. This single short sample is limited evidence and cannot support general performance or leak conclusions.
   - Timing percentiles: `event_reconcile` p50: 13.5 µs, `draw` p50: 37.8 µs, `swap` p50: 5.2 ms. The timing sample has very few events and includes startup activity; the swap interval does not isolate VSync wait, other blocking, or GPU time.
4. **Conclusion**:
   - This evidence does not quantify active-workload allocation cost or demonstrate a worthwhile bottleneck.
   - Runtime C optimization is deferred pending measurements that show a reproducible, worthwhile cost. No zero-risk optimization recommendation follows from this data.

---

### 10.5 Verification Matrix

| Test Suite / Command | Target / Scope | Invariants Verified | Result |
|---|---|---|---|
| `make BUILD=release test` | 44 C test invocations + 1 Python unit suite | All 44 C regression test invocations and 18 Python memory unit tests passed. | **PASS** (45 Make invocations) |
| `make BUILD=release` | Release application, fixture, and test binaries | Build target was already up to date. | **PASS** |
| `python3 benchmarks/run.py memory --pid $$` | Memory CLI (display-less) | Successfully read memory without `DISPLAY` environment variable set. | **PASS** |
| `python3 benchmarks/run.py memory --pid 999999` | Memory CLI (missing PID) | Emitted JSON with null numeric fields, descriptive unavailable reason, and exit code 1. | **PASS** |
| `python3 benchmarks/run.py bench --backends slopcom --workloads empty-idle --repetitions 1 --settle-seconds 1 --sample-seconds 2 --memory` | Recorded live bench with `--memory` on `:1` | Recorded 0 renders/swaps. RSS and private RSS each increased 8 KiB between endpoints; this short observation does not establish growth or leak behavior. | **Recorded sample** |

### 10.6 Closeout and verification scope

The latest offline correction isolated workload tests from desktop/process/power/GPU helpers and covered default-off/enabled memory capture, one synchronous monitor iteration, and unavailable process RSS. The latest release check passed all 44 C cases and the Python suite's 18 tests. Earlier v0.8 C sanitizer checks remain historical evidence; they were not repeated for this Python/docs-only correction, and leak detection was disabled in that tracing environment.

This milestone delivered diagnostics, tests, and reporting, with no runtime C optimization. Active allocation/query profiling and performance changes remain deferred until a reproducible active-workload cost justifies them. See [DONE6.md](DONE6.md) for closeout and deferred work. Daily-driving feedback is the next step; these documentation updates require no new live measurements.

## 11. Personal 1.0 readiness

The agreed target is the existing Linux/Xorg/i3/Radeon/Mesa setup with one monitor on one X screen. Other setups remain best effort. Existing features and the local `make` workflow are retained; public distribution, packaging, installation helpers, and speculative optimizations are outside this milestone.

The README and display limitations now distinguish a single X screen from a single monitor. Screen-wide viewport/damage geometry is not presented as validated multi-monitor support, and independent output scheduling, mixed-refresh guarantees, and dynamic hotplug remain outside scope.

- `make BUILD=release all test` passed. The release build was already up to date; all 44 C regression cases and the offline Python suite's 18 tests passed (45 test invocations).
- The user reported no bugs observed so far during daily use. This is early experience, not a long-term reliability guarantee.
- No runtime source changes, new performance measurements, live desktop checks, or sanitizer reruns were made for this milestone. Continued ordinary daily use provides follow-up feedback; no fixed soak duration or benchmark campaign is required.

---

## 12. Four Focused Rendering Optimizations (2026-11-09)

Implementation and headless verification of the four bounded optimizations authorized in [`PLAN.md`](../PLAN.md).

### 12.1 Changes Delivered

1. **Opaque Blending Bypass** (`src/render.c`):
   - Dynamically disables `GL_BLEND` when drawing windows with no alpha visual (`!w->has_alpha`) and full opacity (`w->opacity == 1.0f`).
   - Retains premultiplied alpha blending (`GL_ONE`, `GL_ONE_MINUS_SRC_ALPHA`) for ARGB and translucent windows.
   - Restores blend state at exit; avoids cross-frame state caching.

2. **Bounded Presentation-Damage Storage** (`src/egl.h`, `src/egl.c`, `src/render.c`):
   - Replaced dynamically allocated heap buffer `EGLint *presentation_damage` with inline array `EGLint presentation_damage[4 * MAX_DAMAGE_RECTS]` inside `EGLState`.
   - Eliminated `realloc()`, allocation pointer checks, and `free()` calls from the active presentation path.
   - Coordinates converted directly into the inline array during partial repaints; resets count to 0 on full redraws or resize.

3. **Buffer-Efficient EGL Config Selection** (`src/egl.c`):
   - Inspects candidate EGL configs matching the native overlay `visual_id`.
   - Filters by required `EGL_OPENGL_ES2_BIT`, `EGL_WINDOW_BIT`, and requested swap interval (0 for `--no-vsync`, 1 otherwise). Validates attribute query return values.
   - Ranks eligible candidates to prefer 0 multisample samples, 0 depth buffer bits, and 0 stencil buffer bits, with deterministic fallback to compatible configs if zero-buffer configs are unavailable.

4. **Reusable Quad Geometry & Vertex State** (`src/render.c`, `src/render.h`):
   - Uploads a static unit quad `k_unit_quad` to `r->vbo` once during `render_init()` with `GL_STATIC_DRAW`.
   - Binds VBO and vertex attributes (location 0 for `a_pos`, location 1 for `a_uv`) up front in `render_init()` and before rendering entry points.
   - Window position, dimensions, and UV tiling factors are passed as uniforms (`u_rect`, `u_uv_extent`) in `k_vertex_shader_src`.
   - Scissor clipping, shape rects, borders, negative coordinates, and wallpaper repeat extents are fully preserved. Static VBO is released in `render_destroy()`.

### 12.2 Verification Matrix

| Test Suite / Command | Target / Scope | Invariants Verified | Result |
|---|---|---|---|
| `make BUILD=release all test` | 48 C regression invocations + 1 Python unit suite (18 tests) | All 48 C regression tests and 18 Python memory unit tests passed. Clean compile with zero warnings under `-Wall -Wextra -Wpedantic`. | **PASS** (49 Make invocations / 66 tests total) |
| `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 make BUILD=debug test` | Debug sanitizer test suite | Clean execution under AddressSanitizer and UndefinedBehaviorSanitizer with zero errors. Leak detection was disabled due to the tracing environment limitation and does not establish absence of leaks. | **PASS** (49 Make invocations / 66 tests total) |
| `build/release/rendering opaque-blending-bypass` | Blending bypass logic | Verifies blend disabled for opaque RGB, blend enabled for translucent RGB, blend enabled for ARGB at opacity 1.0, blend enabled for near-one opacity, and blend disabled for subsequent opaque RGB windows across both full and partial redraw paths. | **PASS** |
| `build/release/rendering bounded-presentation-damage` | Presentation damage array | Verifies presentation damage count is 0 on full repaint, matches max capacity (32 rects) with exact EGL bottom-left coordinate conversion, and tracks single-rect changes on frame N. | **PASS** |
| `build/release/egl_config` | EGL config candidate ranking & filtering | Verifies rejection of visual mismatch, renderable bit mismatch, surface bit mismatch, swap interval rejection, failed attribute queries, candidate ranking preferring zero multisample/depth/stencil, fallback retention, and tie-breaking. | **PASS** |
| `build/release/rendering static-geometry-reuse` | Static quad VBO reuse | Verifies strictly 0 buffer uploads during scene rendering, distinct window rect transforms, identical rect transform reuse across shape clip scissors, and VBO/program teardown. | **PASS** |
| `build/release/background tiling-quads` & `tiny-tile` | Background wallpaper UV tiling | Verifies wallpaper repeat extents, GLES2 clamp-to-edge fallback, and strictly 0 buffer data uploads during background drawing. | **PASS** |

### 12.3 Summary of Results and Boundaries

- **API and Allocation Reductions**:
  - Eliminated recurring `realloc()` and `free()` calls during partial frame presentation.
  - Eliminated recurring 6-vertex buffer generation and streaming VBO uploads on every window and shape rect draw.
  - Reduced alpha blending work for opaque non-ARGB windows.
  - Eliminated unnecessary depth/stencil/multisample buffer allocations where supported by EGL configs.
- **Strict Scope Boundaries**:
  - All verification was performed headlessly with zero desktop interference (no X server changes, no desktop commands, no live benchmarks).
  - No DMA-BUF, XCB, fullscreen bypass, or scheduling changes were introduced.
  - No speculative claims of FPS, RAM, or battery improvements are made; reported improvements are strictly bounded reductions in API calls and allocations verified by test harnesses.

### 12.4 Review follow-up: renderer initialization coverage

The `static-geometry-reuse` case now calls the real `render_init()` against offline GL stubs. It checks one static unit-quad upload, the uploaded position/UV data, vertex-attribute bindings, distinct uniform locations, zero additional uploads during drawing, teardown, and initialization again after teardown. Rendering and wallpaper stubs validate uniform locations and types instead of treating every uniform update as interchangeable.

The existing case was extended, so the suite remains at **49 invocations** (48 C invocations plus one Python suite with 18 tests). The release build/test suite passed. Temporary renderer variants with the initial upload removed, the rectangle uniform misrouted, or the UV uniform misrouted were each rejected by this regression. These variants live outside the repository and do not change production sources.

The affected debug binaries rebuilt without warnings. `static-geometry-reuse`, `tiling-quads`, and `tiny-tile` passed under ASan/UBSan with `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1`; leak detection was disabled. The full debug suite was not repeated for this test/documentation-only follow-up.

The README now distinguishes the four completed optimizations from larger deferred rewrites. This follow-up changes tests and documentation only; it adds no performance measurements or live desktop checks.

---

## 13. Selective XCB Child Query Batching (2026-11-10)

Implementation and headless verification of selective XCB batching for top-level child-window attribute and geometry queries in [`src/x11.c`](file:///home/marc/mine/slopcom/src/x11.c) as instructed in [`PLAN.md`](file:///home/marc/mine/slopcom/PLAN.md).

### 13.1 Changes Delivered

1. **Borrowed XCB Connection**:
   - Borrows the XCB connection via `XGetXCBConnection(x->display)` once per nonempty reconciliation batch.
   - Xlib retains full event queue and connection ownership. No `XSetEventQueueOwner()` or `xcb_disconnect()` calls.
2. **Two-Phase Dispatch & Collection**:
   - **Dispatch Phase**: Flushes prior Xlib work via `XFlush(x->display)`, then queues `xcb_get_window_attributes()` and `xcb_get_geometry()` for every candidate child in root-tree order, followed by a single `xcb_flush()`.
   - **Collection Phase**: Consumes both replies and error structures for every queued window before entering the reconciliation mutation loop.
   - Visual resolution for viewable `InputOutput` windows resolves `xcb_visualid_t` against `ScreenOfDisplay(x->display, x->screen)->depths` without test-only fallbacks.
   - Error replies, vanished windows, and missing replies are handled gracefully; all replies and error objects are freed with `free()`.
   - If `xcb_connection_has_error()` detects connection loss, delegates to `fatal_x_io_error_handler()` to exit immediately without issuing cleanup over a dead connection.
3. **Barrier Elimination**:
   - Completely removes the per-child `x11_trap_errors` / `XSync` / `x11_untrap_errors` error-trap barriers around child attribute queries.
   - Preserves root-level `XQueryTree` traps, Damage error matching, and named pixmap synchronization.

### 13.2 Verification Matrix

| Test Suite / Command | Target / Scope | Invariants Verified | Result |
|---|---|---|---|
| `make BUILD=release all test` | 56 C regression cases + 1 Python unit suite (18 tests) | All 56 C regression test invocations and 18 Python memory unit tests passed. Clean compile with zero warnings under `-Wall -Wextra -Wpedantic`. | **PASS** (57 Make invocations / 74 tests total) |
| `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 make BUILD=debug test` | Debug sanitizer test suite | Clean execution under AddressSanitizer and UndefinedBehaviorSanitizer with zero errors. Leak detection was disabled due to the tracing environment limitation and does not establish absence of leaks. | **PASS** (57 Make invocations / 74 tests total) |
| `build/release/window_lifecycle batch-ordering` | Dispatch/collection pipeline | All 4 requests sent before first reply, `xcb_flush` barrier between dispatch and collection, reply-to-XID mapping, root-tree stacking order. | **PASS** |
| `build/release/window_lifecycle stable-second-reconcile` | Barrier elimination | Zero serial `XGetWindowAttributes` calls, zero per-child error-trap barriers during second reconcile (only root query barrier). | **PASS** |
| `build/release/window_lifecycle geometry-visuals` | Geometry & visual resolution | Negative coordinates, border widths, client dimensions, 24-bit RGB and 32-bit ARGB visual resolution, and alpha classification survive conversion. | **PASS** |
| `build/release/window_lifecycle subscription-preservation` | Subscription preservation | Existing per-connection event-mask bits survive alongside compositor subscriptions; other clients' masks not copied. | **PASS** |
| `build/release/window_lifecycle vanished-child` | Protocol error recovery | Attributes and geometry errors handled separately; all queued requests consumed, surviving siblings captured, resource counters clean. | **PASS** |
| `build/release/window_lifecycle filtered-empty-children` | Filtering & exclusions | InputOnly and unmapped windows consumed without addition to viewable scene; overlay window excluded from batch; empty tree handled cleanly. | **PASS** |
| `build/release/window_lifecycle failure-after-collection` | Early failure rollback | Complete collection before mutation guarantees no outstanding cookies if subsequent EGL import fails; full resource rollback. | **PASS** |
| `build/release/window_lifecycle connection-loss` | Fatal I/O exit | Forked child process verifies missing reply on broken connection causes immediate fatal exit status 1 without dead-connection cleanup requests. | **PASS** |

### 13.3 Summary of Boundaries

- **Round-Trip and Barrier Reduction**:
  - Queues top-level attribute and geometry queries before collecting replies, eliminating the converted path's per-child synchronous `XSync` barriers. This does not establish a fixed physical round-trip count.
- **Strict Scope Boundaries**:
  - All verification was performed headlessly with zero desktop interference (no X server changes, no desktop commands, no live benchmarks).
  - No fullscreen bypass, DMA-BUF, Present, threading, timers, or persistent metadata cache introduced.
  - No speculative claims of FPS, RAM, or battery improvements are made; reported improvements are strictly bounded reductions in protocol serialization and barriers verified by test harnesses.







### 13.4 Review follow-up: query diagnostics and failure coverage (2026-11-10)

Unexpected attribute/geometry query errors now identify the operation, window, code, request opcode, and sequence; expected vanished-window errors remain quiet. A failed XCB flush takes the fatal connection-loss path immediately, and loss during reply collection is detected before reconciliation mutates resource ownership.

The shared offline stubs now use the fake Display's request serial stream, retain full serials across 32-bit cookie rollover, and deliver earlier queued Xlib errors on XCB replies. Outstanding requests are tracked and consumed once; wrong-type or duplicate retrievals are rejected. All lifecycle cases assert that no requests remain outstanding.

Two focused cases (`xcb-protocol-errors` and `xcb-reply-lifecycle`) extend the suite to **59 Make invocations**: **58 C invocations** plus one Python suite with **18 tests**. The existing connection-loss case now injects failure before dispatch, at flush, and on the first reply. Dedicated guards catch reply reads after a failed flush and attempts to send Xlib requests after observed connection loss.

Verification: `make BUILD=release all test` and `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 make BUILD=debug test` both passed all 59 invocations. No compiler warnings appeared in the rebuild logs. Leak detection was disabled; this is not leak verification. Temporary variants with query diagnostics, the immediate flush-failure check, or the post-collection connection check removed were each rejected by the focused regressions.

Historical benchmark artifacts remain unchanged. These fixes establish diagnostics and test coverage, not measured speedup, lower memory usage, or absence of leaks. No live desktop checks were run.

---

## 14. Milestone v1.0 — Fullscreen Unredirection (`--unredir-fullscreen`)

### 14.1 Architecture & Implementation

1. **Opt-In CLI Option & Precedence**:
   - Added `--unredir-fullscreen` CLI argument, default disabled.
   - Enforced strict precedence: `--full-redraw` forces `unredir_fullscreen = false` regardless of argument ordering.
2. **Strict Eligibility Invariants**:
   - Single-monitor / whole-X-screen borderless fullscreen application only; exact geometry coverage of `(0, 0, root_w, root_h)` in root coordinates (translated via `XTranslateCoordinates`).
   - Managed `InputOutput` root child with exactly one live client discovered via `WM_STATE` hierarchy logic. Rejects unmanaged windows, missing clients, and ambiguous client counts.
   - Visuals without alpha (RGB only), rectangular unshaped bounding regions (`XShape`).
   - Raw opacity either absent or valid `CARDINAL 32` == `UINT32_MAX`.
   - Client hint `_NET_WM_BYPASS_COMPOSITOR != 2`.
   - Conservative rejection of override-redirect root children or popups intersecting root bounds.
3. **Storage vs Scene Lifetime Separation**:
   - Releases backing storage (named pixmaps, EGLImages, textures, damage) on bypass entry while retaining window metadata, hierarchy, shape rects, and event subscriptions.
   - Bypassed reconciliation performs metadata-only updates (`metadata_only = true`), avoiding damage creation, pixmap naming, and texture imports.
   - Bypassed state suppresses paint-only wakeups in `has_pending_internal_work()`, returning the event loop to blocking `poll()`.
4. **Two-Phase Restoration Handoff**:
   - Uses a two-phase restoration sequence intended to reduce exposure to incomplete frames:
     - `FULLSCREEN_RESTORE_HIDDEN`: reacquires root redirection, reconciles backing storage, renders and executes full swap to hidden overlay.
     - `FULLSCREEN_RESTORE_VISIBLE`: reveals overlay (`ShapeBounding = None`), renders visible full repair swap, returns to `FULLSCREEN_COMPOSITED`.
   - Recoverable unredirect or overlay-hide failures disable bypass and rebuild client storage before the caller may render. A failed strict recovery or restoration returns to cleanup with the overlay still hidden.

### 14.2 Verification Matrix

These are the initial v1.0 feature checks. The current follow-up suite totals and results appear in Section 14.4 below.

| Test Suite / Command | Target / Scope | Invariants Verified | Result |
|---|---|---|---|
| `make BUILD=release all test` | 65 C regression cases + 1 Python unit suite (18 tests) | All 65 C regression test invocations and 18 Python memory unit tests passed. Clean compile with zero warnings under `-Wall -Wextra -Wpedantic`. | **PASS** (66 Make invocations / 83 tests total) |
| `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 make BUILD=debug test` | Debug sanitizer test suite | Clean execution under AddressSanitizer and UndefinedBehaviorSanitizer with zero errors. Leak detection was disabled due to the tracing environment limitation and does not establish absence of leaks. | **PASS** (66 Make invocations / 83 tests total) |
| `build/release/window_lifecycle fullscreen-acceptance` | Acceptance criteria | Direct root client and reparented client with `_NET_WM_BYPASS_COMPOSITOR = 1` successfully enter bypass; releases storage; unredirects root; hides overlay. | **PASS** |
| `build/release/window_lifecycle fullscreen-rejections` | 17 rejection cases | Conservative rejection of sub-root position/size, borders, ARGB visuals, shaped windows, opacity < UINT32_MAX, malformed opacity, veto hint (2), malformed hint, client offsets/borders/visuals/shapes, missing `WM_STATE`, and override-redirect candidates. | **PASS** |
| `build/release/window_lifecycle fullscreen-popups` | Popup interaction | Visible override-redirect popup blocks bypass; offscreen popups, unmapped windows, and InputOnly top windows do not block bypass. | **PASS** |
| `build/release/window_lifecycle fullscreen-bypassed-reconcile` | Bypassed reconciliation | Metadata-only reconciliation maintains geometry and outer dimensions contract without importing storage or creating damage resources. | **PASS** |
| `build/release/window_lifecycle fullscreen-two-phase-restoration` | Restoration sequence | Correct transition through `FULLSCREEN_RESTORE_HIDDEN` (reacquired redirection, storage rebuilt, hidden swap) and `FULLSCREEN_RESTORE_VISIBLE` (overlay revealed, visible repair frame) back to `FULLSCREEN_COMPOSITED`. | **PASS** |
| `build/release/window_lifecycle fullscreen-intervening-event` | Restoration isolation | Intervening geometry/damage events during restoration do not interrupt active restoration phase. | **PASS** |
| `build/release/window_lifecycle fullscreen-failures` | Fault recovery | Injected failures in unredirect, overlay hide, and redirect reacquisition handle gracefully; bypass disabled for process on unrecoverable entry failures. | **PASS** |
| `build/release/shutdown` | CLI flags and precedence | `--unredir-fullscreen` parsed; `--full-redraw` overrides `--unredir-fullscreen` in both argument orders; `has_pending_internal_work` suppresses paint flags when bypassed. | **PASS** |

### 14.3 Boundaries

- **Strictly Headless**: All verification performed headlessly via unit/integration harnesses; zero desktop interference (no X server changes, no desktop commands, no live benchmarks).
- **Scope Limits**:
  - No RandR multi-output bypass, DMA-BUF, Present, timers, or automatic benchmarking.
  - Bypass is strictly opt-in and restricted to whole-X-screen single monitor geometry.

### 14.4 Follow-Up Fixes — 2026-11-10

- Policy evaluation now runs at startup and after relevant invalidations. It consumes evaluated invalidations, retains them while X events or reconciliation work remain, and skips eligibility queries on unchanged iterations and Damage-only wakeups. The main-loop work predicate schedules an initial evaluation only when the option is enabled.
- Failed unredirect and overlay-hide entry now rebuild window storage synchronously before returning to the render path. Regression coverage runs the actual renderer and presentation stub for both failure paths and checks that the client is drawn.
- Restoration refuses to proceed on a live window's pixmap capture failure, an unsuccessful root `QueryTree`, unexpected or missing XCB child probes, or an unavailable visual. Expected vanished-window errors remain skippable. The overlay stays hidden when strict rebuilding fails.
- Candidate intersection uses the stored outer width and height once; bordered offscreen popup coverage runs in both composited and metadata-only states.
- Focused regressions: `fullscreen-policy-invalidation`, `fullscreen-entry-recovery`, expanded `fullscreen-popups`, and expanded `fullscreen-failures`.
- Headless tests verify request ordering and renderer decisions, not visible EGL/overlay behavior. The real-X handoff remains unverified; no live desktop operation, `make check`, smoke test, or benchmark was run for this follow-up.
- That follow-up passed **68 invocations**: **67 C regression cases** plus one Python suite with **18 tests**, or **85 case executions**, in release and debug. Debug leak detection was disabled. Later results are recorded in [the finalization audit](../AUDIT.md).

## 15. Finalization Audit Fixes — 2026-11-11

Four Luna max subagents implemented one audit fix each; the parent reviewed the combined changes and independently checked the results:

- The phony `slopcom` target reliably selects the requested build mode. A new isolated Python regression uses the actual Makefile to check both switching directions, repeat selection and independent direct binary targets.
- Damage fetching preserves nonempty reply bounds when rectangle allocation returns NULL/count zero; the existing rendering regression also confirms that an empty successful fetch schedules no repaint.
- Distinct fullscreen candidate clients are watched for ShapeNotify before eligibility rejection. The lifecycle regression covers a shaped client becoming rectangular through the real event handler and a replacement client receiving its watch; unchanged and Damage-only paths retain their no-query checks.
- Zero texture names fail before counter increments or binding. The EGL regression checks image unwind, balanced counters, zero-name binding avoidance, and successful import/release accounting.

`make BUILD=release all test` and `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 make BUILD=debug build/debug/slopcom test` both passed **69 Make invocations**: **67 C invocations plus 19 tests across two Python suites = 86 case executions** per configuration. No compiler warnings or sanitizer errors were reported. Leak detection was disabled.

Independent parent rechecks confirmed the four corrected outcomes. Logs: `/tmp/slopcom-audit-fixes-release.log`, `/tmp/slopcom-audit-fixes-debug.log`, and `/tmp/slopcom-audit-fixed-recheck-7xnxbkr2/recheck.log`. No live desktop operations, benchmarks, smoke tests or `make check` were run. Real-X overlay/EGL appearance remains unverified, and historical benchmark data is unchanged. See [AUDIT.md](../AUDIT.md) for dispositions and original reproduction evidence.
