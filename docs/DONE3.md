# DONE3 — v0.6: X11 App and Wallpaper Compatibility

**Project:** Minimal X11 compositor (AMD iGPU + Mesa, EGL/OpenGL ES, VSync, transparency)  
**Version:** `0.6`  
**Builds on:** [DONE1.md](DONE1.md) (v0.1 prototype) and [DONE2.md](DONE2.md) (v0.2–v0.5 reliability/efficiency); consult [validation.md](validation.md) for recorded evidence and limitations.  
**Focus:** Work correctly with normal X11 desktop tools—**especially `xwallpaper`**—while preserving the compositor's zero-idle-frame goal.

> **v0.6 is a compatibility release, not an effects release.** Do not add blur, shadows, rounded corners, fade animations, a configuration framework, or a second renderer. Prioritize widely used X11 behavior, then test it against actual applications.

**Verification budget:** Use `make test`, focused wallpaper regressions, and a short manual smoke test. Check other applications during normal daily use and record failures with reproduction steps. Extended soak tests, benchmark matrices, 100-change stress loops, and multi-output experiments are optional; do not make them prerequisites for implementing this release.

## Definition of done (P0)

- [x] A wallpaper set by `xwallpaper` appears *behind* all normal windows, including under transparent terminal windows.
- [x] Changing the wallpaper **while the compositor is running** updates the image without restarting the compositor.
- [x] A wallpaper set *before* compositor startup is detected and displayed at startup.
- [x] The background remains stable during workspace changes, moving/resizing windows, and opening/closing popups.
- [x] Common X11 apps (terminal, launcher, menu, panel, notification daemon, browser) remain visible, correctly stacked, clickable, and focusable.
- [x] ARGB transparency, whole-window opacity, and VSync continue to work as in earlier versions.
- [x] Static wallpaper + unchanged windows cause **zero frame swaps** while idle; there is no polling to detect wallpaper changes.
- [x] Missing, invalid, or abruptly destroyed background pixmaps do not crash the compositor or leave broken X11 resources.
- [x] A documented compatibility test matrix passes on the target AMD iGPU + Mesa + Xorg + i3 machine.

## Scope and constraints

**Required:** Standard wallpaper-pixmap properties; root-background rendering; correct order of normal/override-redirect windows; launcher/notification/panel compatibility; basic single-screen/root resize handling.

**Nice to have:** Multi-monitor layout changes, per-output wallpaper tests, rotated outputs, testing a second window manager.

**Not promised:** Every program that manually paints pixels directly onto the root window (with no published pixmap), every proprietary X11 extension, every mixed-refresh-rate configuration, or arbitrary pseudo-transparency schemes. Document these separately rather than adding expensive polling or CPU readbacks.

### Compatibility principle: the compositor does not own the wallpaper

- **`xwallpaper` is an independent app.** It sets the root background and normally advertises an X pixmap through `_XROOTPMAP_ID` and/or `ESETROOT_PMAP_ID`.
- The compositor should **read and display that pixmap**, not launch `xwallpaper`, decode the image file, or take ownership of the wallpaper's X resources.
- `_XSETROOT_ID` is *not* the wallpaper image; it can be a dummy resource ID. It can help signal a background change but must not be interpreted as the image pixmap.
- A property's pixmap ID can become stale between reading and importing it. Handle X11 errors and keep an opaque fallback background.
- **Use the same imported EGL texture path where possible**, but validate that *arbitrary root-background pixmaps*, not just XComposite-named window pixmaps, can be imported successfully on the AMD/Mesa target.

---

## Phase 1 — Make wallpapers visible (highest priority)

### 1.1 Root background ownership and events

- [x] Keep the real X root window separate from the normal redirected top-level-window list.
- [x] Track a `background_state` with a pixmap ID, dimensions/depth, optional EGLImage/texture, validity, and refresh-required flag.
- [x] Subscribe to root `PropertyChangeMask` **without clobbering existing selected event masks** (when using Xlib, use `XGetWindowAttributes` + OR the new mask in `XSelectInput` for this client).
- [x] Intern/read `_XROOTPMAP_ID`, `ESETROOT_PMAP_ID`, and `_XSETROOT_ID`.
- [x] On compositor startup, query the wallpaper properties and populate background state before the first composited frame.
- [x] On root `PropertyNotify` for any relevant atom, invalidate the background state and schedule **one** redraw after coalescing pending events.
- [x] Prefer a valid `_XROOTPMAP_ID` pixmap, then `ESETROOT_PMAP_ID`; verify property type (`XA_PIXMAP`), format (`32`), and one-element length.
- [x] Treat `_XSETROOT_ID` only as a change hint; do not try to import it as wallpaper.
- [x] On invalid/missing properties, use a defined fallback color and log a diagnostic only in verbose mode.

### 1.2 Safely acquire, import, and draw wallpaper pixmaps

- [x] Check the pixmap really exists and query its geometry/depth before import.
- [x] Use the existing EGL pixmap-to-EGLImage-to-GLES texture path *only after* verifying support for this pixmap type/visual on the actual driver.
- [x] Do **not** call `XFreePixmap` on pixmaps owned by `xwallpaper`, `feh`, or another application. Destroy only your own GL/EGL wrapper resources.
- [x] Refresh imported texture contents when a wallpaper update is advertised **even if the pixmap ID stays the same** (a setter may reuse the pixmap).
- [x] Coalesce refresh requests and replace bindings only during scene reconciliation, not while drawing. Keep ownership of the published X pixmap external on replacement, failure, and shutdown.
- [x] Handle `BadPixmap`, `BadDrawable`, failed imports, and replacement during acquisition without crashing. Fall back and retry on the next relevant change.
- [x] Draw the wallpaper **first**, followed by windows in bottom-to-top stacking order using the existing blending path.
- [x] Share one background-drawing function between the full-redraw and partial-redraw paths. Clear an opaque fallback first, then draw valid wallpaper within the active repaint clip; preserve that clip while drawing windows. Ordinary window damage must repaint wallpaper beneath translucent or newly exposed areas, not the old solid background.
- [x] Preserve root coordinates and image orientation; avoid stretching or flipping the wallpaper accidentally.
- [x] Compare wallpaper pixmap dimensions with root dimensions. Full-screen pixmaps should cover the root; when a smaller pixmap is intentionally used as a background tile, repeat it instead of implicitly scaling it to fit.
- [x] Handle non-power-of-two tiles on GLES2 explicitly. Use `GL_REPEAT` only when the context supports repeating those textures (for GLES2, check `GL_OES_texture_npot`); otherwise draw repeated quads using the existing clamp-to-edge texture path. Test an odd-sized tile, not only power-of-two images.
- [x] Keep the direct GPU import contract: no second full-screen image or CPU-copy fallback in v0.6. Use the opaque fallback for unsupported pixmaps and document the limitation.
- [x] Confirm transparent terminal backgrounds reveal the wallpaper, not an opaque black fallback.

### 1.3 Startup and runtime tests (`xwallpaper`)

- [x] **Startup test:** run `xwallpaper --zoom /path/to/wallpaper.jpg` *before* starting the compositor, then start it; image appears correctly.
- [x] **Hot-change test:** with compositor running, switch between two distinct wallpapers a few times; every change appears promptly and without black flashes. A 20-change stress run is optional.
- [x] Test `--center`, `--zoom`, and `--tile` with a small checkerboard/test-pattern image; check positioning and tiling.
- [x] Test wallpaper changes while a transparent terminal overlaps the desktop.
- [x] Test `xwallpaper --clear` as a valid black-background update, separately from failure handling; it initializes the background rather than proving that missing properties are handled.
- [x] Add focused controlled-response tests for missing/malformed properties, a stale or removed pixmap, and failure during import/replacement. Expect an opaque fallback, usable windows, and no attempt to free an externally owned X pixmap. Revalidate on the next relevant event; do not poll to discover silent external destruction.
- [x] Stop and restart the compositor without restarting `xwallpaper`; new instance loads the current background.
- [x] Inspect properties with `xprop -root _XROOTPMAP_ID ESETROOT_PMAP_ID _XSETROOT_ID` during debugging.
- [x] If already available, smoke-test a second root-property wallpaper setter, such as `feh --bg-fill`, to catch accidental `xwallpaper` special cases. Otherwise record it as untested.

**Exit gate:** The wallpaper works before and after compositor startup and survives repeated runtime changes; no polling, memory growth, or resource ownership errors.

---

## Phase 2 — Keep other X11 apps working

### 2.1 Window discovery, lifetime, and stacking

- [x] Distinguish root, overlay, composited top-level windows, and any internal X11 helper windows.
- [x] Track **override-redirect** windows (menus, popups, tooltips, some launchers), not just windows managed by i3.
- [x] Respect actual X server stacking order; do not invent a new order solely based on `_NET_WM_WINDOW_TYPE`.
- [x] Handle windows already mapped at startup and windows that are mapped/reparented/reconfigured/restacked later.
- [x] Correctly handle `MapNotify`, `UnmapNotify`, `DestroyNotify`, `ReparentNotify`, `ConfigureNotify`, and `CirculateNotify`.
- [x] Account for frame/decorations, border widths, and shaped windows where applicable; do not accidentally draw duplicate client/frame windows.
- [x] Refresh stale named pixmaps on resize and validate resource lifetimes during rapid show/hide.
- [x] Ensure transparent overlays and popup windows compose over everything beneath them without black rectangles.
- [x] Maintain an empty input shape on the compositor overlay. It must **never** intercept focus, keyboard, or pointer events.
- [x] Detect an existing composite manager and fail gracefully rather than competing for `_NET_WM_CM_S<n>`.

### 2.2 App compatibility checks

- [x] **Terminals:** test one terminal with transparency and one fully opaque terminal (e.g. `xterm`, `kitty`, `alacritty` if installed).
- [x] **Launchers:** test `rofi` and/or `dmenu`; menu opens above other windows, typing and selection work, focus returns normally.
- [x] **Notifications:** test `dunst` + `notify-send`; notifications appear, disappear, and do not leave stale pixels.
- [x] **Panels:** test `polybar` or another dock/panel; proper stacking, clicking, visibility during workspace/fullscreen changes.
- [x] **Browsers:** test Firefox window, context menus, tooltips, and video playback.
- [x] **i3 workflows:** tiled/floating windows, scratchpad, workspace switching, fullscreen, urgent/focused windows.
- [x] **Screen capture:** verify `scrot` or an equivalent tool captures the expected visible desktop, and note any X11 capture limitations.
- [x] When investigating a compatibility problem, optionally compare the same workflow with picom as a correctness reference, **not** as proof of the most efficient design. Installing/running a second compositor is not a routine verification prerequisite.

**Exit gate:** No missing popups, stuck input, incorrect stacking, stale textures, or app-specific compositor crashes in the test matrix.

---

## Phase 3 — Display and wallpaper geometry

- [x] Read root-window size and handle root `ConfigureNotify` when XRandR changes the screen dimensions.
- [x] Update EGL viewport and projection/scissor coordinates without a persistent render loop.
- [x] Revalidate wallpaper pixmap geometry after root resize and after root wallpaper-property changes.
- [x] Cover a root-size change with a focused regression test. A manual supported resolution switch is optional; restore the previous mode if testing it.
- [ ] **Optional multi-output follow-up:** check root-relative texture coordinates, monitor plug/unplug, and `xwallpaper --output <name> --zoom <file>` where hardware permits. Keep these outside the required single-screen gate.
- [ ] **Optional XRandR awareness:** observe output configuration only when implementing that follow-up. Root `ConfigureNotify` is sufficient for the required root-size handling; do not add an XRandR dependency merely for VSync.
- [x] Document whether mixed refresh rates/rotations are supported, untested, or known problematic; `eglSwapInterval(1)` alone is not proof of per-output synchronization.

**Exit gate:** No black margins, offset wallpaper, incorrect crop/tiling, EGL errors, or permanent artifacts after supported display changes.

---

## Phase 4 — Protect reliability and ultra-low overhead

- [x] All wallpaper and window updates are **event-driven**; a static wallpaper does not cause render calls or swaps.
- [x] Coalesce root-property events and XDamage events into a single scene update when possible.
- [x] Treat wallpaper changes as a full-background invalidation (or a correct equivalent), then restore damage-aware updates for normal activity.
- [x] Reuse the wallpaper texture while valid; no per-frame pixmap/EGLImage recreation.
- [x] Avoid CPU pixel readback and unconditionally uploading whole-screen wallpaper every frame.
- [x] Avoid `glFinish()`, extra round trips, and synchronous calls on the hot path unless correctness demands them.
- [x] Destroy old EGLImage/GL bindings safely after replacing the wallpaper; ensure the compositor does not retain stale background allocations.
- [x] Log invalid pixmap/texture failures only when necessary; don't flood logs or spin retrying a bad resource.
- [x] Run `make test` with the new wallpaper cases, then a short smoke test covering startup wallpaper, a few hot changes, transparent-window movement/exposure, and stop/restart. Confirm both optimized and `--full-redraw` modes show the expected background; automated pixel-comparison infrastructure is not required.
- [x] Take render/swap snapshots before and after a short motionless interval (about 10 seconds). Counts must remain constant; disable cursor blink/animations for this check. No timer is added to the compositor.
- [x] Check owned-resource counters return to baseline after replacement/fallback and to zero after shutdown. Watch for obvious resource growth during daily use; extended RSS/GPU/XID sampling is optional.

**Exit gate:** Existing and focused wallpaper tests pass, short idle snapshots show no render/swap increase, and replacement/shutdown resource accounting is balanced. Longer stress runs are optional rather than a release prerequisite.

---

## Suggested Arch Linux test tools

Install only tools you intend to test. These are *test clients*, not dependencies of the compositor itself:

```sh
sudo pacman -S --needed \
  xwallpaper feh xorg-xprop xorg-xwininfo xorg-xrandr \
  xorg-xsetroot xterm rofi dmenu dunst libnotify
```

For optional app tests, use your existing terminal, browser, and panel or install the relevant packages individually.

### Minimal manual smoke test

Run the following inside your **real Xorg/i3 session** (not Wayland/Xwayland), with any other compositor stopped. Adjust image paths and use an installed transparent terminal for the alpha check. Run `make test` first.

```sh
# 1. Establish a wallpaper, then start the compositor.
xwallpaper --zoom "$HOME/Pictures/wallpaper.jpg"
xprop -root _XROOTPMAP_ID ESETROOT_PMAP_ID
./slopcom --verbose

# 2. In another terminal, change the wallpaper while it is running.
xwallpaper --center "$HOME/Pictures/another-wallpaper.jpg"

# 3. Launch typical X11 clients.
xterm &
rofi -show drun
notify-send 'Compositor test' 'Check notification placement'

# 4. Inspect X windows and display state if a failure occurs.
xwininfo -root -tree
xrandr --query
```

**Caution:** `xwallpaper --no-atoms` and tools that paint directly to the root without publishing an accessible pixmap cannot be detected reliably using root pixmap `PropertyNotify` alone. Similarly, plain `xsetroot -solid` may not publish a usable image pixmap. Do not claim they work without an explicit test and a separate documented fallback design. Do not solve this by constantly capturing the root window.

## Test matrix (record actual outcomes)

| Scenario | Expected result | Pass? |
|---|---|---|
| Wallpaper before compositor startup | Background appears | [x] |
| Wallpaper set while compositor runs | Immediate visible update | [x] |
| A few wallpaper changes | No crash / stale frame / resource leak | [x] |
| Small odd-sized wallpaper tile | Repeats correctly, no stretching | [x] |
| Valid black background from `--clear` | Black wallpaper displays normally | [x] |
| Transparent terminal over wallpaper | Correct alpha/background | [x] |
| Rofi/dmenu over terminal | Correct order, focus, input | [x] |
| Dunst notification over fullscreen/normal app | No disappearance or stuck overlay | [x] |
| Panel + i3 workspace switches | Correct placement, no artifacts | [x] |
| Firefox popups and video | Correct compositing/stacking | [x] |
| Root-size regression test | Correct dimensions, background, and repaint invalidation | [x] |
| Static wallpaper + idle windows | No frame swaps | [x] |
| Invalid/missing wallpaper pixmap | Stable opaque fallback | [x] |
| Stop/restart compositor | Wallpaper persists, resources cleaned | [x] |

## Release gate: v0.6

Do **not** tag v0.6 until every P0 definition-of-done item is checked, the test matrix is recorded on the AMD/Mesa machine, and no new performance/stability regression is observed against v0.5.

Use brief smoke checks and daily-use observations for the app matrix; mark unavailable apps as untested with a reason and do not claim universal compatibility. Optional stress tests, additional compositors/setters, and multi-output work do not block this gate. A known reproducible P0 failure does block it.

Keep unresolved compatibility gaps in `KNOWN_LIMITATIONS.md` with precise reproduction steps. Do not silently add CPU-heavy workarounds just to make a special case appear to work.

## Technical references

- [xwallpaper upstream](https://github.com/stoeckmann/xwallpaper) — publishes root wallpaper and supports output-specific wallpapers.
- [xwallpaper source (`main.c`)](https://github.com/stoeckmann/xwallpaper/blob/master/main.c) — root background pixmap, atoms, and `xcb_clear_area` behavior.
- [xwallpaper manual](https://github.com/stoeckmann/xwallpaper/blob/master/xwallpaper.1) — `--clear` and image placement modes.
- [Khronos GLES NPOT texture support](https://registry.khronos.org/OpenGL/extensions/OES/OES_texture_npot.txt) — requirements for repeating non-power-of-two textures.
- [picom X11 source](https://github.com/yshui/picom/blob/next/src/x.c) — root pixmap lookup / background property handling.
- [Arch XSetRoot property notes](https://man.archlinux.org/man/extra/perl-x11-protocol-other/X11%3A%3AProtocol%3A%3AXSetRoot.3pm.en) — `_XROOTPMAP_ID`, `ESETROOT_PMAP_ID`, and `_XSETROOT_ID` semantics.
- [XComposite API](https://manpages.ubuntu.com/manpages/focal/man3/XCompositeNameWindowPixmap.3.html) — composite overlay window behavior.
