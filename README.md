# slopcom

A lightweight, zero-idle-overhead X11 compositing manager written in C11 using Xlib, EGL, and OpenGL ES 2.0.

`slopcom` is designed from the ground up for minimal resource usage, strict event-driven execution, and zero wakeups when the screen is idle.

---

## Key Features & Architecture

- **Strict Event-Driven Idle**: Blocks in `poll()` with zero timer wakeups and zero presentation swaps when the scene is unchanged.
- **External Wallpaper Compatibility**: Reads published root background pixmaps (`_XROOTPMAP_ID`, `ESETROOT_PMAP_ID`, `_XSETROOT_ID`) from setters like `xwallpaper` and `feh`. Supports live hot-reloading, tiling, and valid black `--clear` without polling or wakeups.
- **Direct GPU Pixmap Import**: Zero-copy window redirection using `EGL_KHR_image_pixmap` / `GL_OES_EGL_image`. No CPU buffer copies or redundant texture uploads.
- **Safe External Resource Ownership**: Never calls `XFreePixmap` on external setter resources; only internal GL/EGL wrappers are managed and released.
- **Fine-Grained Damage Tracking**: Client-side damage accumulation tracking root-relative dirty bounding boxes across window lifecycle events (move, resize, restack, opacity, shape changes).
- **EGL Buffer Age & Presentation Damage**: Uses supported buffer-age and swap-with-damage extensions to repair dirty regions, with full repaint when preserved content is unavailable.
- **Occlusion Culling**: Detects fully occluded underlying windows and skips texture binding and drawing; drains underlying server damage notifications without triggering presentation swaps.
- **Window Shape & Transparency**: Supports 32-bit ARGB per-pixel visual alpha (revealing wallpaper directly beneath translucent terminals), non-rectangular window bounding shapes (`XShape`), and `_NET_WM_WINDOW_OPACITY` (including i3 frame and client window detection).
- **Opaque Blending Bypass**: Disables blending dynamically for fully opaque non-ARGB windows, avoiding unnecessary alpha blend overhead while preserving painter's order and premultiplied blending for translucent content.
- **Bounded Presentation Damage Storage**: Inline damage storage sized to maximum repair capacity eliminates per-frame heap allocation churn on the active presentation path.
- **Reusable Quad Geometry & Vertex State**: Uploads a single unit quad VBO once at initialization and binds vertex attributes up front, positioning and sizing windows and tiling UVs via GLES2 uniforms (`u_rect`, `u_uv_extent`) to eliminate streaming per-draw vertex buffer uploads.
- **Selective XCB Child Query Batching**: Borrows the Xlib connection via Xlib-XCB to pipeline top-level window attribute and geometry queries in two explicit phases (dispatch then collect), eliminating per-child synchronous error-trap barriers.
- **Buffer-Efficient EGL Config Selection**: Inspects candidate config attributes and prioritizes configurations without unnecessary multisample, depth, or stencil allocations while matching native visual and swap interval constraints.
- **On-Demand Diagnostics**: JSON counters and elapsed timing snapshots on `SIGUSR1`, exit summaries, and a separate process-memory inspection command.
- **Deterministic Correctness Fallback**: `--full-redraw` CLI mode provides a golden reference fallback to verify partial update correctness.
- **Optional Fullscreen Unredirection**: Whole-screen compositor bypass (`--unredir-fullscreen`) for eligible single-screen borderless applications. Restoration uses two full repaint phases; visible handoff behavior has not yet been checked on a real X server.

---

## Validated Target Environment

`slopcom` is tested and verified on:
- **Architecture**: Linux x86_64 (Kernel 7.2.14)
- **GPU Hardware**: AMD Phoenix1 (Radeon 780M / Ryzen 7000/8000 series iGPU)
- **Graphics Driver**: Mesa 26.2.6 (`radeonsi`, ACO, OpenGL ES 3.2, EGL 1.5)
- **Display Server**: X.Org Server 21.1.25
- **Window Manager**: i3 4.25.1
- **Compositor Selection**: `_NET_WM_CM_S0` (XComposite manual redirection)

The personal 1.0 milestone targets this Linux/Xorg/i3/Radeon/Mesa setup with one monitor on one X screen. Other setups are best effort. It keeps the current features and local `make` workflow; packaging, installation helpers, and broader compatibility remain deferred. Release readiness means a passing release build and existing tests, consistent documentation, and continued ordinary daily use with no reported blocker. No fixed soak duration or new benchmark campaign is required.

---

## Performance and memory

See [bench.md](bench.md) for the short performance chart and [v0.9 closeout](docs/DONE6.md) for current status. One recorded memory snapshot measured **154.7 MiB RSS**, **16.9 MiB private RSS**, and **41.6 MiB PSS**; **89.1% of RSS was shared resident pages**. Private RSS includes library/driver pages. These measurements do not establish a leak, total GPU memory usage, or battery savings.

Four focused rendering optimizations are implemented: opaque blending bypass, fixed presentation-damage storage, EGL config ranking, and reusable quad geometry. Their performance impact has not been measured; larger rewrites remain deferred pending evidence of an active-workload bottleneck. The historical performance samples and newer memory snapshot have separate conditions recorded in the [validation notes](docs/validation.md).

Inspect an already-running process without connecting to X or changing the desktop:

```sh
python3 benchmarks/run.py memory --pid 12345
python3 benchmarks/run.py memory --pid 12345 --mappings
```

Replace `12345` with the chosen compositor PID. Missing measurements are reported as JSON `null` with reasons.

---

## Build Dependencies

Development libraries and headers required:
- `x11`
- `x11-xcb`
- `xcb`
- `xcomposite` (>= 0.3)
- `xdamage`
- `xfixes` (>= 2.0)
- `xext` (Shape extension >= 1.1)
- `xrender`
- `egl` (with `EGL_KHR_image_pixmap` support)
- `glesv2` (with `GL_OES_EGL_image` support)

On Arch Linux:
```sh
sudo pacman -S base-devel libx11 libxcb libxcomposite libxdamage libxfixes libxext libxrender mesa
```

---

## Building

Build the optimized release binary (`-O2`):
```sh
make
```

Build the debug binary with AddressSanitizer and UndefinedBehaviorSanitizer (`-O1 -fsanitize=address,undefined`):
```sh
BUILD=debug make
```

Clean build artifacts:
```sh
make clean
```

---

## Running and Command-Line Options

```sh
slopcom [OPTIONS] [DISPLAY]
```

### Options

| Flag | Description |
|---|---|
| `-h`, `--help` | Show usage options and exit (code 0). |
| `--verbose` | Enable verbose diagnostic logging to stderr. |
| `--stats` | Enable detailed high-resolution internal latency timing (event reconcile, draw, swap). |
| `--full-redraw` | Force full scene repaint on every dirty frame over a fresh background (golden reference correctness mode). |
| `--no-vsync` | Disable VSync presentation synchronization via `eglSwapInterval(0)` (diagnostic mode). |
| `--unredir-fullscreen` | Bypass compositing when an eligible fullscreen application covers the root (disabled by default; overridden by `--full-redraw`). |
| `DISPLAY` | Optional target X11 display (e.g. `:0`, `:1`). Defaults to `$DISPLAY`. |

### Starting `slopcom`

Verify no other compositor is running:
```sh
xprop -root _NET_WM_CM_S0
```

Start the compositor:
```sh
./slopcom
```

Or via Makefile:
```sh
make run
```

---

## Telemetry and Diagnostics

Send `SIGUSR1` to print a live JSON snapshot of resource counts, swap statistics, and elapsed timing distributions to stdout without restarting the compositor:

```sh
kill -USR1 $(pidof slopcom)
```

Example snapshot:
```json
{
  "schema_version": 1,
  "kind": "snapshot",
  "seq": 1,
  "pid": 12345,
  "time_ns": 10201658633197,
  "x_events": 1420,
  "damage_events": 1286,
  "render_attempts": 1286,
  "successful_swaps": 1286,
  "full_repaints": 2,
  "partial_repaints": 1284,
  "skipped_presentations": 18,
  "skipped_occluded_damage": 4,
  "poll_wakeups": 1302,
  "imports": 12,
  "reimports": 2,
  "x_round_trips": 184,
  "live_pixmaps": 4,
  "live_images": 4,
  "live_textures": 4,
  "live_damage": 4,
  "live_regions": 0
}
```

---

## Automated Testing & Verification Suite

1. **Unit & Controlled-Response Lifecycle Tests**:
   Runs **69 Make invocations**: 67 C regression cases plus two offline Python suites with **19 tests** (18 memory tests and one build-selection test; **86 case executions** total). Covers lifecycle races, wallpaper, partial clipping, config selection, quad geometry reuse, selective XCB batching, resource counters, fullscreen policy and restoration, build selection, and isolated diagnostic tooling without needing an active desktop:
   ```sh
   make test
   BUILD=debug make test
   ```

   Python 3 is required for the offline suite. If LeakSanitizer is unavailable under tracing, use `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 BUILD=debug make test` and record that leak detection was disabled.

2. **Real-X Reliability Verification**:
   Exercises selection collision, 10 start/stop cycles, ARGB/GL/Shape fixtures, scripted churn, and i3 interactions:
   ```sh
   make check
   ```

3. **Wallpaper & App Compatibility Smoke Tests**:
   Verifies startup wallpaper detection, hot changes, tiling, `--clear`, the 10-second zero-idle invariant, and app compatibility (`dunst`, `dmenu`, `xterm`, `alacritty`):
   ```sh
   python3 tools/make_test_wallpapers.py
   python3 benchmarks/smoke_wallpaper.py
   python3 benchmarks/smoke_apps.py
   ```

4. **Performance & Resource Benchmarks**:
   Run the 6 standard benchmark workloads (`empty-idle`, `overlapping-idle`, `typing`, `drag-resize`, `scrolling`, `video`):
   ```sh
   python3 benchmarks/run.py bench --fast --backends slopcom slopcom-full
   ```

5. **Optional Long-Run Soak Test**:
   Samples RSS, file descriptors, GPU utilization, and tracked resource counts. Use for a specific unresolved stability question; it is not required for routine verification and cannot establish absence of every leak:
   ```sh
   # Quick 60-second smoke soak
   python3 benchmarks/run.py soak --quick

   # Full 4-hour soak test
   python3 benchmarks/run.py soak --duration 14400 --sample-interval 30
   ```

---

## Documentation & Evidence

- [`docs/DONE1.md`](docs/DONE1.md) — Prototype milestone v0.1.
- [`docs/DONE2.md`](docs/DONE2.md) — Reliability and efficiency roadmap (v0.2–v0.5).
- [`docs/DONE3.md`](docs/DONE3.md) — X11 app and wallpaper compatibility (v0.6).
- [`docs/DONE4.md`](docs/DONE4.md) — Repaint efficiency and Damage lifecycle fixes (v0.7).
- [`docs/DONE5.md`](docs/DONE5.md) — Focused module and ownership cleanup (v0.8).
- [`docs/DONE6.md`](docs/DONE6.md) — Memory diagnostics and evidence-based optimization deferrals (v0.9).
- [`docs/validation.md`](docs/validation.md) — Comprehensive validation matrix and recorded telemetry.
- [`KNOWN_LIMITATIONS.md`](KNOWN_LIMITATIONS.md) — Architectural boundaries and compatibility edge cases.

---

## Known Limitations

- **Display Support**: The validated target is one monitor on one X screen. Multiple monitors sharing that screen are best effort; independent per-output scheduling, mixed-refresh guarantees, and dynamic XRandR hotplug reconfiguration are outside the 1.0 scope. See [display limitations](KNOWN_LIMITATIONS.md#2-display-layouts-and-refresh-rates).
- **Direct GPU Import Required**: Assumes hardware acceleration via `EGL_KHR_image_pixmap`. CPU copy fallbacks are omitted by design to maintain low energy usage.
- **Root Wallpaper Property Required**: External wallpaper setters must publish a pixmap atom (`_XROOTPMAP_ID` or `ESETROOT_PMAP_ID`). Setters using `--no-atoms` or direct root window painting without publishing a pixmap will trigger the stable fallback color. See [`KNOWN_LIMITATIONS.md`](KNOWN_LIMITATIONS.md) for details.
- **Minimalist Pipeline**: No window shadows, blur, or animations.
- **Fullscreen Unredirection Scope**: Whole-screen compositor bypass (`--unredir-fullscreen`) is opt-in, restricted to single-screen borderless windows matching root geometry, and conservatively rejects translucent, shaped, or override-redirect popups. See [`KNOWN_LIMITATIONS.md`](KNOWN_LIMITATIONS.md#4-fullscreen-unredirection---unredir-fullscreen).
- **Selection Exclusivity**: Adheres to the ICCCM/EWMH `_NET_WM_CM_Sn` selection standard. Will refuse startup if another compositor owns the selection.

---

## Uninstalling and Disabling

To stop `slopcom`, send `SIGTERM` (or press `Ctrl+C` in the terminal):
```sh
kill -TERM $(pidof slopcom)
```

Upon termination:
- Unbinds EGL context and surfaces.
- Releases all imported pixmaps, textures, and damage listeners.
- Unredirects the root window and destroys the composite overlay window.
- Releases `_NET_WM_CM_S0` ownership.
- Restores the desktop cleanly to uncomposited X11 rendering with zero artifacts.
