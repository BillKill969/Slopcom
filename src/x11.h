#ifndef SLOPCOM_X11_H
#define SLOPCOM_X11_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/shape.h>
#include "egl.h"
#include "background.h"
#include "damage.h"
#include "x11_errors.h"

enum ChangeFlags {
    CHANGE_NONE        = 0,
    CHANGE_CONTENT     = 1 << 0,
    CHANGE_HIERARCHY   = 1 << 1,
    CHANGE_GEOMETRY    = 1 << 2,
    CHANGE_PROPERTY    = 1 << 3,
    CHANGE_SHAPE       = 1 << 4,
    CHANGE_OUTPUT      = 1 << 5,
};

#define X11_EVENT_BATCH_CAP 256
#define MAX_DAMAGE_REQUESTS X11_EVENT_BATCH_CAP

typedef struct {
    Damage damage;
    unsigned long serial;
    bool visible;
    bool failed;
} DamageRequest;

typedef enum {
    FULLSCREEN_COMPOSITED = 0,
    FULLSCREEN_BYPASSED,
    FULLSCREEN_RESTORE_HIDDEN,
    FULLSCREEN_RESTORE_VISIBLE,
} FullscreenMode;

typedef struct {
    Window xid, opacity_client;
    int x, y, width, height, border_width, depth;
    VisualID visual_id;
    bool viewable, storage_dirty, has_alpha, opacity_dirty;
    bool override_redirect;
    float opacity;
    Damage damage;              /* Owned association, or None after retirement. */
    Pixmap pixmap;              /* Owned named pixmap; release after import wrappers. */
    ImportedPixmap imported;   /* Owned EGLImage/texture; EGL must be current. */
    bool shaped;
    XRectangle *shape_rects;    /* Owned heap allocation for non-rectangular shape clip. */
    int num_shape_rects;
    bool is_occluded;
} WindowState;

typedef struct X11State {
    Display *display;
    int screen;
    Window root;
    int root_width;
    int root_height;
    int root_depth;
    VisualID root_visual_id;

    Window owner_win;
    bool selection_acquired;
    Atom net_wm_cm_s;
    Atom manager_atom;
    Atom net_wm_opacity;
    Atom wm_state_atom;

    bool redirected;
    Window overlay;
    bool overlay_acquired;
    VisualID overlay_visual_id;

    FullscreenMode fullscreen_mode;
    bool unredir_fullscreen;
    bool fullscreen_recheck;
    bool overlay_hidden;
    Window bypass_frame;
    Window bypass_client;
    Atom net_wm_bypass_compositor;

    int composite_event_base, composite_error_base;
    int damage_event_base, damage_error_base;
    int damage_opcode;
    DamageRequest damage_requests[MAX_DAMAGE_REQUESTS];
    size_t damage_request_count;
    int fixes_event_base, fixes_error_base;
    int shape_event_base, shape_error_base;

    WindowState *windows;
    size_t num_windows;
    size_t cap_windows;

    uint32_t pending_changes;
    bool pending_reconcile;
    bool root_size_changed;
    bool scene_dirty;
    bool full_redraw;
    bool needs_full_repaint;

    XserverRegion scene_damage;
    DamageList client_damage;
    DamageList client_history[DAMAGE_HISTORY_CAP];
    int history_count;
    int history_head;

    BackgroundState background;

    struct Stats *stats;
} X11State;

static inline bool x11_repaint_pending(const X11State *x) {
    return x->needs_full_repaint || x->client_damage.count > 0;
}

/* Core X11 interface */
bool x11_init(X11State *x, const char *display_name);
bool x11_reconcile(X11State *x, EGLState *e);
void x11_handle_event(X11State *x, XEvent *ev, bool *dirty, bool *stop_requested);
/* Complete and retire asynchronous DamageSubtract checks after one event batch. */
void x11_finish_damage_batch(X11State *x);
/* Call while EGL is current, before egl_destroy; safe to call again. */
void x11_release_windows(X11State *x, EGLState *e);
void x11_destroy(X11State *x, EGLState *e);

WindowState *x11_find_window(X11State *x, Window w);

bool x11_damage_rect(X11State *x, int rx, int ry, int rw, int rh);
bool x11_damage_window(X11State *x, WindowState *w);
void x11_update_occlusion(X11State *x);
void x11_invalidate_full_scene(X11State *x);

/* Fullscreen bypass hooks */
bool x11_reconcile_and_advance_fullscreen(X11State *x, EGLState *e, bool *dirty);
bool x11_finish_fullscreen_presentation(X11State *x, EGLState *e, bool *dirty);

#endif /* SLOPCOM_X11_H */
