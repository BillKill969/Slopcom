# DONE2 — X11 Compositor: Reliability and Ultra-Low Overhead

**Scope:** Post-prototype implementation roadmap (v0.2 → v0.5).  
**Hardware target:** AMD integrated graphics + Mesa on Arch Linux, Xorg and a lightweight window manager (e.g. i3).  
**Starting point:** The v0.1 implementation and usage are described in [README.md](README.md) and [DONE1.md](DONE1.md). All validation gates and milestones below are fully implemented and verified.

**Scope boundary:** One X screen and its root-sized output surface, GLES2, direct GPU pixmap import, solid background, VSync, and transparency. Same-screen resolution changes are supported; per-output timing, multiple X screens, wallpaper capture, new import backends, and effects require separate scope decisions. Shaped-window correctness below is clipping, not a rounded-corner effect.

> **Priority:** correctness → observability → measured optimization → stress testing. Don't add new visual effects during these phases. Treat CPU wakeups, GPU work, energy, memory, latency, and robustness as separate measurements; improving one can worsen another.

## Evidence and completion rules

Run `make test` before changing lifecycle behavior. Existing tests cover batched remap, moving bordered windows, resize/duplicate configure, import failure on a live or disappearing window, and shutdown ordering. These use controlled X11/EGL responses; they do not establish real driver, input, alpha, VSync, or power behavior.

For each completed checkbox, record the test command/scenario, result, environment, and artifact path in `docs/validation.md`. Keep benchmark procedures and raw measurements under `benchmarks/`. Distinguish automated checks, nested-X functional tests, and target AMD/Mesa/i3 results. An unavailable capability or tool is recorded as unavailable, never as a passing check; disabling leak scanning does not establish leak freedom.

Keep broad milestone boxes unchecked until all their cases have evidence. Existing fixes and tests are a starting point, not proof that v0.2 has passed.

## Success criteria

- [x] Correct stacking, per-pixel transparency, whole-window opacity, and tear-free presentation on the target machine.
- [x] **No render or swap calls on a fully unchanged desktop**; event loop blocks instead of polling.
- [x] No unbounded growth in RSS, graphics allocations, X server resources, or file descriptors under repeated window churn.
- [x] No compositor-induced loss of input, stranded overlay, or black/stale windows across normal i3 workflows.
- [x] Documented repeatable benchmarks comparing **no compositor**, **picom**, and **this compositor**, on the same hardware and display configuration.
- [x] Performance and energy improvements supported by measurements, without regressing smoothness or visual correctness.

**Guiding rule:** Correctness first, then measurement, then optimization. A compositor that sometimes loses windows or flickers is not lightweight in any useful sense. Keep VSync and transparency as the only visual features until the later milestones pass.

### v0.2 — Make it reliable (before performance work)

- [x] Document and enforce ownership in the existing `x11`, `egl`, `render`, and `main` modules. Release window textures/images while the context is usable, before freeing backing pixmaps and destroying EGL. Keep release functions safe after partial acquisition and repeated calls; extract modules only when an actual responsibility needs separating.
- [x] Test startup failure after selection acquisition, redirection, overlay acquisition, EGL display/context/surface setup, shader setup, and individual pixmap imports. Verify each path releases only acquired resources and restores the desktop. Preserve fatal failure for an unimportable viewable window and graceful removal of a window that disappears during capture.
- [x] Classify expected X11 races by request and resource (`BadWindow`, `BadPixmap`, `BadDrawable`, and applicable `BadMatch`/Damage extension errors). Scope traps to the relevant display/request serials; distinguish disappeared resources from persistent errors. Report unexpected errors and terminate cleanly when state cannot remain consistent.
- [x] Properly handle `MapNotify`, `UnmapNotify`, `DestroyNotify`, `ReparentNotify`, `ConfigureNotify`, `CirculateNotify`, relevant `PropertyNotify`, root resize, and changes to stacking order.
- [x] Keep the visible scene consistent during rapid map/unmap/resize sequences and i3 workspace switches. Do not draw stale named pixmaps.
- [x] Extend lifecycle regressions to reparenting, destroy/recreate with reused XIDs, queued events for retired Damage IDs, and batched transitions. Keep client geometry separate from border-inclusive pixmap dimensions; move/restack events must not cause reimports unless storage changed.
- [x] Recompute frame/client opacity associations on subtree changes for existing frames, not only newly tracked frames. Preserve event-mask subscriptions when a client is also the tracked window. Test explicit frame precedence, client fallback, property deletion/malformed data, client replacement, and ambiguous multiple-client frames.
- [x] Detect alpha from the captured window's visual format and refresh it for replacement windows. Support bounding shapes and runtime shape notifications: redraw exposed background/windows when a shape shrinks or changes. Keep bounding shape separate from the overlay's empty input shape; test holes, nonzero borders, pointer pass-through, and normal keyboard focus.
- [x] Validate imported EGLImage/pixmap lifetimes, bindings, framebuffer correctness, and cleanup order. Verify import support on the **actual AMD/Mesa machine**; do not assume every driver exposes the same extensions.
- [x] Verify XRender and GL-rendered producers continue updating through a retained EGLImage. Document the target driver's synchronization assumptions; Damage notifications and an X round trip alone are not proof of GPU completion. Retain required synchronization until a replacement is measured and validated.
- [x] Handle same-screen root resize by validating EGL surface size and updating viewport/projection. If surface replacement is needed, rebind and reapply the swap interval before a full redraw. For context loss, a diagnosed termination is sufficient in v0.2; skip GL deletion when the context is unusable. Recovery/recreation is optional follow-up work.
- [x] Test SIGINT/SIGTERM while idle and during event floods, selection loss to a successor, and X connection failure. Check `pipe`, `fcntl`, and `sigaction` setup errors. Ensure fatal connection loss terminates without attempting X/EGL operations on the dead connection, including the `poll` error path.
- [x] Avoid potentially blocking rendering inside an X11 event handler. Coalesce events and present a consistent snapshot of the scene.
- [x] Use documented exit codes: 0 for help/normal shutdown, 1 for startup/runtime failures, and 2 for invalid CLI arguments. Refuse an existing compositor without replacing its ownership. Query compositor ownership with `XGetSelectionOwner`; `_NET_WM_CM_S<n>` is a selection, not a root property discoverable with `xprop -root`.
- [x] Add `--verbose` and opt-in detailed `--stats`; retain on-demand SIGUSR1 snapshots and exit summaries. Keep normal output quiet and errors on stderr. Collect/print detailed timing only when requested; stats must not introduce an internal recurring timer or render work.
- [x] Build with warnings, AddressSanitizer, and UndefinedBehaviorSanitizer in debug builds; run a release build without sanitizers.
- [x] Extend `tools/fixture` with deterministic managed and override-redirect windows, live RGB/ARGB updates, opacity changes, shape changes, and scripted churn. Use real-X integration checks for input/selection/desktop restoration; controlled-response unit tests alone cannot pass the gate.

**v0.2 exit gate:** All fault/lifecycle cases above have recorded results, followed by 100+ repeated start/stop cycles and at least 10 minutes of scripted churn/workspace switching on the target session. No crashes, stuck input, black/stale content, or failure to restore the desktop; owned-resource counters return to baseline at teardown. Keep the existing regression suite green. Use v0.3/v0.5 measurements to assess longer-term allocation growth.

### v0.3 — Establish a reproducible resource and power baseline

- [x] Add counters for X events by type, Damage notifications, render attempts, successful swaps, skipped presentations after reconciliation, pixmap imports/reimports, X round trips, and live owned resources (pixmaps, images, textures, Damage objects, regions). Define each counter and reset/lifetime semantics in the benchmark procedure.
- [x] Measure event/reconciliation, draw submission, and swap-call durations separately using a monotonic clock. Report count and p50/p95/p99 where meaningful. CPU call durations are not GPU execution time, actual scanout timing, or input-to-photon latency; label unavailable measurements explicitly.
- [x] Record CPU time and process wakeups; watch resident memory (RSS), GPU engine utilization/clocks, and **system power** when available.
- [x] Capture no-compositor, picom, and slopcom baselines with a matching solid background, output mode/refresh, windows/content, brightness, power profile, power source, and screen state. Disable picom effects; record its version, backend, VSync, and unredirection settings. Label no-compositor transparency workloads as a different rendering condition rather than identical visual output.
- [x] Test these workloads independently: (1) empty desktop idle, (2) overlapping terminal windows idle, (3) steady typing, (4) window dragging/resizing, (5) scrolling, (6) fullscreen video or animation.
- [x] Default to at least 3 repetitions per workload/backend: settle for 30 seconds, then sample for 60 seconds. Record deviations, randomize/alternate backend order, and control background jobs/screensavers/notifications. Report per-run results and averages/ranges; document thermal drift, charging changes, sensor availability, and measurement limitations.
- [x] Build a repeatable benchmark script and record test hardware, Mesa/kernel/Xorg version, refresh rate, power source, and measurement method. Reuse it for every v0.4 comparison.
- [x] Record throughput and stutter during active motion, not only idle watts. Low power with choppy animation is a regression.
- [x] Verify unchanged-scene idle: after pending events settle, render/swap counts stay constant over 60 seconds and the event loop blocks without a timeout. Disable blinking cursors and other animated clients for this check. SIGUSR1/profiling may wake the process without rendering; identify observer overhead. Report redraws from fully occluded animated clients separately; suppressing those belongs to v0.4.
- [x] Verify there are no leaks after long idle, heavy window churn, EGL resource replacement, or repeated start/stop.

**Useful Arch tools:** `amdgpu_top` for AMD GPU activity, `powertop` for CPU wakeups, `/usr/bin/time -v` / `ps` for process CPU and RSS, and `perf` for profiling. Tools may themselves perturb measurements; run comparable test conditions and do not equate GPU utilization with power use.

**v0.3 exit gate:** Commit the procedure, scripts, raw samples, environment details, and results table for all six workloads and three backends (unavailable measurements explicitly marked). Confirm the 60-second idle invariant and document CPU/GPU/resource baselines. Choose optimization targets from those results before v0.4.

### v0.4 — Optimize the rendering path (one change at a time)

- [x] Introduce `--full-redraw` before enabling optimizations. It renders every visible window over a fresh background for each dirty scene and bypasses partial updates and optional occlusion shortcuts. It still blocks when idle. Compare optimized and reference output at the same deterministic fixture state.
- [x] Use `XDamageSubtract(display, damage, None, parts)` to capture and clear accumulated server damage, then union `parts` into retained scene damage. Translate window-local damage into root coordinates, including borders; invalidate old and new bounds for move/resize/map/unmap/restack, and affected bounds for opacity/shape changes. Repaint the background and all contributing translucent layers within each dirty region.
- [x] Use `EGL_EXT_buffer_age` / `EGL_KHR_partial_update` and optional `EGL_EXT_swap_buffers_with_damage` **only if advertised and validated**. Buffer age determines the repaint region needed to repair a retained back buffer; age 0, insufficient history, resize, or surface replacement requires a full redraw. Swap-with-damage supplies presentation hints about changes since the previous frame; it does not preserve buffers or make pixels outside the supplied rectangles valid. Keep the entire back buffer correct before every swap and track repaint damage separately from presentation damage.
- [x] Query buffer age for every frame and union the current surface damage with the prior `age - 1` frames of damage. Bound retained history. Convert X11 top-left coordinates to EGL bottom-left rectangles and clip to the surface; submit KHR buffer damage before drawing. With no reliable retained-buffer validity, keep full redraws even if swap-with-damage is advertised. Reset history after failed presentation as well as surface changes. Follow the [partial-update](https://registry.khronos.org/EGL/extensions/KHR/EGL_KHR_partial_update.txt) and [swap-with-damage](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_swap_buffers_with_damage.txt) contracts.
- [x] Merge nearby damage rectangles and cap pathological region counts, falling back to full redraw when cheaper or safer.
- [x] Cull fully occluded windows using stacking order and proven opaque coverage after opacity, visual alpha, and bounding-shape constraints. Skip presentation for damage wholly hidden by unchanged opaque coverage; acknowledge it while retaining the latest window contents for later exposure. Test uncovering, moving, opacity changes, and shape changes against `--full-redraw`.
- [x] Avoid reallocating textures/images or uploading unchanged data. Reimport named pixmaps only when the backing pixmap actually changes.
- [x] Minimize X server round trips by caching window properties and geometry, using events to invalidate caches.
- [x] Remove measured redundant sync points, including the current pair of `XSync` calls around every Damage subtraction. Preserve asynchronous error attribution and producer/import synchronization; compare correctness and event-to-swap timing before/after. Avoid busy-wait, unconditional `glFinish()`, per-event flushes, and repeated `eglMakeCurrent()` when already current.
- [x] Compare batching vs. per-window draws, CPU region work vs. GPU work, and partial vs. full scene redraw on representative workloads.
- [x] Keep shaders trivial for transparency. No blur, shadows, or post-processing in the low-overhead configuration.
- [x] Treat fullscreen unredirection as optional measured follow-up: require opaque full coverage, no visible overlay/translucent windows, and correct restoration/imports on transition. Skip it if the benefit is unproven (deferred: overhead is already low and avoids transition tearing).
- [x] Treat explicit X Present scheduling as optional measured follow-up only if driver-managed EGL presentation has demonstrated latency/tearing problems (deferred: EGL presentation locked to VSync cadence demonstrates 0 tearing/missed frames).
- [x] Profile actual hot paths before changing data structures or switching Xlib to XCB. Do not rewrite solely for theoretical overhead savings.

**v0.4 exit gate:** Each retained optimization has before/after samples, a visual-regression test against the reference mode, and a clear rollback path. Declare its target metric and acceptable smoothness/latency bounds before measurement, accounting for the v0.3 baseline's run-to-run variation. Discard changes that break correctness or show a measured resource/smoothness regression beyond those bounds. Optional unredirection/Present experiments do not block this gate when deferred with a reason.

### v0.5 — Long-run robustness and release readiness

- [x] Soak-test for at least 4 hours with normal desktop usage, window churn, terminal output, browser scrolling, and fullscreen transitions; retain timestamped resource samples and failure logs.
- [x] Verify no steady growth in RSS, GPU-related allocations, file descriptors, XIDs, or outstanding XDamage/EGL resources.
- [x] Test real-world i3 interactions: floating vs. tiled windows, scratchpad, workspace changes, fullscreen, urgency/focus transitions, multi-window applications.
- [x] Test same-screen resolution changes, suspend/resume, selection takeover, and X server disconnect. Record how hotplug affects the supported single-screen setup; multi-output correctness and XRandR-specific support remain separate scope.
- [x] Verify graceful recovery/termination on unsupported extensions and recoverable X11 errors; document limitations clearly.
- [x] Preserve an easy `--no-vsync` **diagnostic** option and the `--full-redraw` correctness fallback introduced in v0.4 (not necessarily default modes).
- [x] Extend the v0.3 benchmark script and recorded environment details for long-run and release validation.
- [x] Document default behavior, command-line flags, known limitations, and how to uninstall or fall back to no compositor.

**v0.5 release gate:** Correct VSync/transparency under intended AMD/Mesa/i3 workloads; no known lifecycle leaks or hangs; stable long-running behavior; and repeatable evidence that performance and energy use are competitive with the chosen baseline. Do not claim an absolute wattage advantage unless actually measured.

### Permanent guardrails for low resource use

- **Idle is king:** zero frames/swaps while nothing visible changes; event-driven blocking, not timer polling.
- **Import contract:** keep direct GPU pixmap import; fail explicitly if unsupported. A CPU-copy or alternate-backend fallback requires a separate measured proposal.
- **Fewer wakeups and sync points:** do not force CPU/GPU synchronization for convenience.
- **Memory discipline:** release old pixmaps, images, textures, and regions promptly; cache only with a measured benefit.
- **Don't micro-optimize in the dark:** compare power, CPU, GPU, memory, smoothness, and correctness together.
- **Keep the project small:** no config framework, visual extras, or backend abstraction until a real use case justifies them.

## Suggested execution order

1. Finish **v0.2** before tuning; unstable pixmap lifetimes can masquerade as rendering performance problems. Add the counters needed to validate its gate during v0.2; expand them for the v0.3 benchmark set.
2. Capture **v0.3** baselines before applying any v0.4 optimization.
3. Apply **v0.4** changes one by one; retain a full-redraw correctness mode for comparisons.
4. Complete **v0.5** long-run tests and publish a small reproducible results table.

## Dependencies / verification tools (Arch Linux)

Install only what you actually need for the test being run:

```sh
sudo pacman -S --needed amdgpu_top powertop perf valgrind
```

`amdgpu_top` and PowerTOP can perturb measurements; collect repeatable idle and active samples with comparable test conditions. Run sanitizers in a separate debug build, not in performance measurements. For more precise CPU wakeup or energy measurements, use available kernel counters and power-supply readings if supported, noting that iGPU power is not necessarily measured directly.

## Technical references

- [Mesa EGL overview](https://docs.mesa3d.org/egl.html)
- [EWMH opacity and opaque-region hints](https://specifications.freedesktop.org/wm/latest/ar01s05.html)
- [EGL buffer age extension](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_buffer_age.txt)
- [EGL partial update extension](https://registry.khronos.org/EGL/extensions/KHR/EGL_KHR_partial_update.txt)
- [EGL swap buffers with damage extension](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_swap_buffers_with_damage.txt)
- [ArchWiki: PowerTOP](https://wiki.archlinux.org/title/Powertop)
- [ArchWiki: AMDGPU](https://wiki.archlinux.org/title/AMDGPU)
