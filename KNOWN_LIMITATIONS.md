# Known Limitations

This document tracks known architectural boundaries and compatibility edge cases in `slopcom`.

---

## 1. Direct-to-Root Rendering without Published Pixmap (`xwallpaper --no-atoms`, `xsetroot`)

- **Description**: Tools or commands that paint directly onto the X11 root window (or use `--no-atoms`) without publishing a standard Pixmap XID to `_XROOTPMAP_ID` or `ESETROOT_PMAP_ID` cannot be detected via `PropertyNotify` alone. Similarly, plain `xsetroot -solid` modifies root window background pixel attributes without publishing a queryable Pixmap property.
- **Behavior in slopcom**: When no published pixmap property exists or properties are deleted, `slopcom` displays a stable opaque fallback color (`#1f242b`) without crashing or leaking resources.
- **Resolution / Workaround**: Use standard X11 wallpaper setters that publish pixmap atoms (such as `xwallpaper` without `--no-atoms`, `feh --bg-fill`, or `nitrogen`). `slopcom` does not poll the root window or perform expensive CPU readbacks (`XGetImage`) to discover non-standard root modifications.

---

## 2. Display Layouts and Refresh Rates

- **Validated target**: One monitor on one X screen, using Linux/Xorg/i3 with Radeon/Mesa. Historical recordings include 2880 × 1760 and 2880 × 1800 at 60 Hz; their original conditions remain in [validation notes](docs/validation.md).
- **Multiple monitors**: An X screen can span several monitors. Viewport/damage geometry is screen-wide, but that does not establish validated multi-monitor compatibility. Such layouts and other hardware/window-manager combinations remain best effort for the personal 1.0 milestone.
- **Refresh and hotplug**: `slopcom` uses EGL VSync (`eglSwapInterval(1)`) for the composited screen. It does not independently schedule each output or guarantee mixed-refresh behavior. Dynamic XRandR hotplug reconfiguration is outside the 1.0 scope.

---

## 3. Pseudo-Transparency Schemes

- **Description**: Applications that implement fake/pseudo-transparency by copying root window pixels on startup rather than using standard 32-bit ARGB visuals (`XA_CARDINAL` opacity or `PictTypeDirect` alpha channel) will not composite dynamically.
- **Resolution / Workaround**: Use real 32-bit ARGB transparency in terminals (`alacritty`, `kitty`, `xterm` with ARGB visual). Real alpha blending is hardware-accelerated directly via OpenGL ES 2.0.

---

## 4. Fullscreen Unredirection (`--unredir-fullscreen`)

- **Opt-in Status**: Disabled by default; must be explicitly enabled with `--unredir-fullscreen`.
- **Precedence**: Specifying `--full-redraw` forces fullscreen unredirection to be disabled regardless of argument ordering.
- **Single-Screen Restriction**: Designed strictly for single-monitor / whole-X-screen borderless fullscreen applications that exactly cover the root window coordinates `(0, 0, root_w, root_h)`. Multi-monitor spanning, per-output bypass, or partial-screen unredirection is not supported.
- **Conservative Eligibility**: Requires an opaque rectangular non-shaped `InputOutput` root child window with visuals without alpha, exactly 1 live client window (discovered via `WM_STATE`), zero border width, raw opacity either absent or valid CARDINAL 32 == UINT32_MAX, and no `_NET_WM_BYPASS_COMPOSITOR = 2` veto. Reject any override-redirect top window intersecting the root, ambiguous client hierarchies, or missing clients.
- **Handoff Synchronization**: Restoration reacquires root redirection while the overlay remains hidden, rebuilds backing storage, and requests a full hidden-overlay frame. It then reveals the overlay and requests a second full repair frame. This ordering is intended to reduce incomplete-frame exposure; headless tests cannot establish whether every EGL/X server combination presents it without visible flicker. The real-X handoff has not been validated.
