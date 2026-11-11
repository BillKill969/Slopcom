# DONE1 — Milestone v0.1 (Initial Working Compositor)

Milestone v0.1 delivered the initial prototype of `slopcom`:

- **Event-Driven Foundation**: Core Xlib event loop with XComposite manual redirection (`_NET_WM_CM_S0`).
- **Direct GPU Import**: Zero-copy window pixmap texture import via `EGL_KHR_image_pixmap` / `GL_OES_EGL_image`.
- **Transparency Support**: 32-bit ARGB visual alpha channels and `_NET_WM_WINDOW_OPACITY` detection.
- **VSync Synchronization**: Presentation locked to display refresh rate via `eglSwapInterval(1)`.
- **Overlay Window Management**: Transparent composite overlay window rendering.
