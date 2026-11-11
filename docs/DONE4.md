# TODO4.md — v0.7: Less Redundant Work

**Objective:** Fix Damage lifecycle races and avoid unnecessary frames and X server requests while preserving wallpaper, transparency, VSync, and normal X11 application behavior.

Keep the existing EGL + OpenGL ES renderer. Prefer small changes with observable benefits. Short automated checks and daily use are sufficient for this milestone; extended benchmarks and soak tests are optional.

## Existing behavior to preserve

These mechanisms already exist. Extend or repair them where needed rather than implementing replacements:

- Blocking `poll()` on the X connection and signal pipe, with no render timer.
- Event batches limited to 256 events so continuous traffic cannot prevent presentation indefinitely.
- Screen-clipped damage accumulation and old/new window footprint invalidation.
- Conservative whole-window occlusion and offscreen draw skipping.
- Runtime EGL capability checks, bounded buffer-age history, partial repaint, and swap-with-damage.
- Full repaint fallback and `--full-redraw` for comparison.
- Pixmap import reuse, live wallpaper updates, and GPU wallpaper repetition where supported.
- JSON statistics and existing lifecycle, rendering, shutdown, and wallpaper tests.

Source map: `src/main.c` owns scheduling; `src/x11.c` owns events and window state; `src/render.c` and `src/egl.c` own repaint and presentation; `src/stats.*` and `benchmarks/run.py` own diagnostics.

## 1. Handle stale Damage resources safely (P0)

The reported `BadDamage` from request minor opcode 3 identifies `XDamageSubtract`. Both subtraction paths currently lack error handling; a resource can become invalid while its notification is still queued.

- [x] Handle stale Damage IDs in both the visible-window extraction path and the occluded-window acknowledgement path.
- [x] Ignore notifications for retired windows/resources. Retire local associations consistently when a window is destroyed or a Damage resource is confirmed invalid.
- [x] Handle the race where a resource disappears after local validation. Checking the local window list alone is insufficient.
- [x] Limit expected-error handling to the relevant request/resource. Preserve diagnostics for unrelated X errors; do not silence `BadDamage` globally.
- [x] On failed extraction, do not consume stale scratch-region contents. Schedule reconciliation or conservative repaint when visible state is uncertain.
- [x] Keep cleanup and resource counters correct after server-side destruction and repeated local release.
- [x] Avoid adding two synchronous `XSync()` calls to every Damage notification. Keep error handling localized and account for any required synchronization.

**Done when:** Controlled stale-resource cases exercise both subtraction paths without an unhandled error, stale repaint data, or leaked local resources. Ordinary damage on live windows still produces updates.

The implementation records each `XDamageSubtract` by display-local request serial and Damage XID. The error callback only marks an exact matching request as failed; it does not touch window records or issue X requests. A visible `XFixesFetchRegionAndBounds` reply completes the relevant records. Trailing occluded acknowledgements share one `XSync()` at the end of the bounded event batch. Reconciliation recreates a missing Damage object in place without reimporting its pixmap; a persistent `XDamageCreate` failure aborts reconciliation.

## 2. Submit frames only for useful repaint work (P0)

Currently, Damage handling can set `dirty` even when clipping adds no screen damage. `render_scene()` then treats an empty damage list as a reason for full repaint. This can turn offscreen activity into full-screen rendering.

- [x] Distinguish an empty repaint region from an explicit requirement to repaint the full scene. Make scheduling and rendering agree on that distinction.
- [x] Acknowledge offscreen or fully occluded content damage without scheduling a frame solely for that update.
- [x] Handle empty extracted Damage regions without introducing a redundant swap.
- [x] Avoid unconditional repaint flags for lifecycle/property events whose reconciliation produces no visible change. Preserve damage already accumulated from other events in the batch.
- [x] Preserve mandatory initial repaint, full invalidation after root-size or wallpaper changes, and full repair when buffer history is unusable for a required frame.
- [x] Preserve old/new footprints, exposed content, stacking, opacity, shape, and client-replacement updates. Moving an offscreen or occluded window into view must show its latest contents.
- [x] Keep `--full-redraw` event driven: it chooses how a required frame is painted, without causing frames when no update is needed.

**Done when:** Offscreen/occluded updates and unrelated events cause no renders or swaps, while visible updates and explicit invalidations still present correctly. No pixel readback or image comparison is needed to decide whether to schedule a frame.

Overlay `Expose` sets explicit full-repaint intent while preserving any accumulated client damage. `render_scene()` reports whether it drew a required frame, and `--full-redraw` selects the draw mode only after repaint work exists. The main loop checks pending internal recovery after `XPending()` so an error-only recovery request cannot enter blocking `poll()`.

## 3. Make efficiency counters trustworthy (P1)

Extend existing statistics; keep cheap counters available without `--stats`, and retain the flag for detailed timing. Diagnostics must not introduce periodic polling.

- [x] Add a documented count of event-loop poll wakeups, distinguishing X readiness, signal readiness, and interruption as needed. Define these as application loop wakeups, not total OS/thread wakeups.
- [x] Add full-repaint and partial-repaint counts at the renderer's actual branch points.
- [x] Define skip counters consistently. `skipped_presentations` currently mixes skipped event batches and individual occluded Damage events; separate those meanings or document distinct counters.
- [x] Audit `x_round_trips`: it currently omits synchronization and property-query paths. Define exactly which operations are counted; do not describe an API-call count as a precise network round-trip measurement.
- [x] Rename or accurately label the benchmark's current `wakeups` metric: it measures voluntary plus involuntary context-switch deltas.
- [x] Update JSON output, consumers, and counter documentation together. Preserve existing render/swap/resource counters.

`x_round_trips` is a count of selected reply-waiting Xlib operations, not a packet-level round-trip measurement. It does not count every `XSync()` performed by scoped error traps or batch completion.

**Done when:** A snapshot explains frames painted, swaps completed, and work skipped without conflating events, batches, context switches, or poll wakeups. Snapshot signals themselves may wake the loop and must be accounted for when interpreting idle results.

## 4. Reduce measured X11 overhead (P1)

Use corrected counters or a focused trace to choose a small change. Keep this section bounded; leave it deferred if no safe, useful improvement is demonstrated.

- [x] Capture a short before/after comparison for one affected workload, such as window movement or a burst of Damage events, using the same configuration and workload.
- [x] Inspect repeated opacity reads, client-subtree discovery, and synchronization during reconciliation. Refresh metadata when relevant events invalidate it instead of repeatedly querying unaffected state, where safe.
- [x] Preserve frame/client opacity precedence, hierarchy subscriptions, client replacement, and resource lifetime ordering when introducing reuse or caching.
- [x] Preserve bounded event processing and the existing safe damage-list limit. Improve rectangle merging only if it reduces measured repaint work without excessive extra area or complexity.
- [x] Record the change and observed request/frame reduction in `docs/validation.md`. Defer changes that add complexity without a demonstrated benefit.

Do not remove synchronization required for pixmap import or resource lifetime safety merely to improve a counter. Avoid new frame timers, per-event blocking checks, and forced GPU completion.

## 5. Efficient verification and completion

- [x] Add focused regression cases to the existing test binaries for stale Damage on both subtraction paths, empty/offscreen damage, and repaint after previously hidden content becomes visible.
- [x] Test that skipping one event does not erase another event's visible damage, and that explicit full invalidation still paints with otherwise empty damage.
- [x] Exercise delayed `BadDamage` delivery during the XFixes reply and at the trailing hidden-damage batch barrier; verify exact request matching, deferred state mutation, Damage recreation, and no pixmap reimport.
- [x] Exercise overlay Expose through the renderer/presentation gate, error-only recovery through the main loop, and maximal detailed/human stats output.
- [x] Run `make test` and build the release binary. Run the sanitizer suite once for lifecycle/error-handling changes; repeat only after relevant changes or failures.
- [x] When a usable X session is available, check a settled static desktop for about 10 seconds: render and swap counters must remain unchanged. Account for diagnostic signals and active application updates.
- [x] Perform one brief visual check of affected behavior: move/resize/close a window, uncover content, and inspect transparency or wallpaper if those paths changed. Reuse existing smoke tests where useful; avoid a mandatory full application matrix.
- [x] Record commands, results, and any untested behavior in `docs/validation.md`. Update README or limitations only where behavior or diagnostics changed.

**Release criteria:** The confirmed Damage race is handled, irrelevant/offscreen updates no longer force frames, existing tests pass, visible changes repaint correctly, and efficiency claims match the recorded evidence. Existing partial rendering, VSync, and compatibility remain intact.

A short idle check establishes the tested interval, not an indefinite guarantee. GPU utilization is not a power measurement. Longer energy comparisons may be run separately when reliable telemetry and repeatable conditions are available; they do not block these fixes.

## Out of scope

- Fullscreen unredirection or direct scanout.
- New rendering backends, effects, threading, or complex frame scheduling.
- RandR hotplug/per-output scheduling, mixed-refresh guarantees, or new EGL context-loss recovery. Preserve existing root-size handling.
- Pixel-perfect detection of identical application redraws, root-image readback, or aggressive partial-occlusion algorithms.
- RAM micro-optimization, mandatory five-minute idle runs, soak tests, and benchmark matrices.

**Implementation order:** Damage lifecycle fix → repaint scheduling fix → accurate counters → one justified X11 overhead reduction → short verification and documentation.
