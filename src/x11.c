#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/damagewire.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xlib-xcb.h>
#include <xcb/xcb.h>

#include "x11.h"
#include "x11_errors.h"
#include "egl.h"
#include "stats.h"

typedef struct {
    Window xid;
    xcb_get_window_attributes_cookie_t attributes_cookie;
    xcb_get_geometry_cookie_t geometry_cookie;
    XWindowAttributes attributes;
    bool queued;
    bool valid;
} WindowProbe;

static void log_window_query_error(Window window, const char *query,
                                   const xcb_generic_error_t *error,
                                   unsigned char expected_error) {
    if (!error || error->error_code == BadWindow || error->error_code == expected_error)
        return;
    fprintf(stderr, "slopcom: XCB %s failed for window 0x%lx: error %u, request %u.%u, sequence %u\n",
            query, (unsigned long)window, (unsigned int)error->error_code,
            (unsigned int)error->major_code, (unsigned int)error->minor_code,
            error->full_sequence);
}

static Visual *lookup_visual_by_id(Display *dpy, int screen, VisualID id) {
    Screen *s = ScreenOfDisplay(dpy, screen);
    if (!s || !s->depths) return NULL;
    for (int d = 0; d < s->ndepths; ++d) {
        Depth *depth = &s->depths[d];
        if (!depth->visuals) continue;
        for (int v = 0; v < depth->nvisuals; ++v) {
            if (depth->visuals[v].visualid == id) {
                return &depth->visuals[v];
            }
        }
    }
    return NULL;
}

static WindowState *find_window_by_damage(X11State *x, Damage dmg);

static bool serial_before(unsigned long a, unsigned long b) {
    return (long)(a - b) < 0;
}

static void retire_damage_association(X11State *x, Damage damage, bool repaint_window) {
    WindowState *w = find_window_by_damage(x, damage);
    if (w) {
        if (repaint_window && x11_damage_window(x, w))
            x->scene_dirty = true;
        w->damage = None;
        if (x->stats && x->stats->live_damage > 0) x->stats->live_damage--;
    }
}

static bool complete_damage_requests_before(X11State *x, unsigned long fence_serial,
                                            unsigned long *target_serial) {
    bool current_failed = false;
    size_t kept = 0;
    for (size_t i = 0; i < x->damage_request_count; ++i) {
        DamageRequest request = x->damage_requests[i];
        if (!serial_before(request.serial, fence_serial)) {
            x->damage_requests[kept++] = request;
            continue;
        }
        if (request.failed) {
            if (target_serial && request.serial == *target_serial)
                current_failed = true;
            if (request.visible && x->scene_damage != None)
                XFixesSetRegion(x->display, x->scene_damage, NULL, 0);
            retire_damage_association(x, request.damage, request.visible);
            x->pending_reconcile = true;
        }
    }
    x->damage_request_count = kept;
    return current_failed;
}

static bool register_damage_request(X11State *x, Damage damage, bool visible,
                                    unsigned long *serial_out) {
    if (x->damage_request_count == MAX_DAMAGE_REQUESTS)
        x11_finish_damage_batch(x);

    if (x->damage_request_count == MAX_DAMAGE_REQUESTS) return false;
    unsigned long serial = NextRequest(x->display);
    DamageRequest *request = &x->damage_requests[x->damage_request_count++];
    *request = (DamageRequest){ .damage = damage, .serial = serial, .visible = visible };
    if (serial_out) *serial_out = serial;
    XDamageSubtract(x->display, damage, None, visible ? x->scene_damage : None);
    return true;
}

void x11_finish_damage_batch(X11State *x) {
    if (!x || !x->display || x->damage_request_count == 0) return;
    unsigned long fence_serial = NextRequest(x->display);
    XSync(x->display, False);
    (void)complete_damage_requests_before(x, fence_serial, NULL);
}

static Damage create_damage_resource(X11State *x, Window drawable, bool *vanished) {
    if (vanished) *vanished = false;
    XErrorTrap trap;
    x11_trap_errors_scoped(x->display, &trap);
    unsigned long request_serial = NextRequest(x->display);
    Damage damage = XDamageCreate(x->display, drawable, XDamageReportNonEmpty);
    int caught = x11_untrap_errors_scoped(&trap);
    if (!caught) return damage;

    bool expected_vanished_window = trap.display == x->display &&
        trap.serial == request_serial && trap.request_code == (unsigned char)x->damage_opcode &&
        trap.minor_code == X_DamageCreate && trap.resourceid == drawable &&
        trap.error_code == BadDrawable;
    if (expected_vanished_window && vanished) *vanished = true;
    if (!expected_vanished_window && trap.prev_handler) {
        XErrorEvent ev = {
            .type = 0, .display = trap.display, .serial = trap.serial,
            .error_code = trap.error_code, .request_code = trap.request_code,
            .minor_code = trap.minor_code, .resourceid = trap.resourceid,
        };
        (void)trap.prev_handler(trap.display, &ev);
    }
    return None;
}

static int fatal_x_io_error_handler(Display *dpy) {
    (void)dpy;
    fprintf(stderr, "slopcom: Fatal X11 I/O error: connection lost\n");
    _exit(1);
}

static bool check_visual_has_alpha(Display *dpy, Visual *visual) {
    if (!visual) return false;
    XRenderPictFormat *format = XRenderFindVisualFormat(dpy, visual);
    if (!format) return false;
    return (format->type == PictTypeDirect && format->direct.alphaMask != 0);
}

static bool read_opacity(X11State *x, Window w, Atom atom, float *out) {
    Atom type = None;
    int format = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char *data = NULL;
    *out = 1.0f;
    x11_trap_errors(x->display);
    if (x->stats) x->stats->x_round_trips++;
    int rc = XGetWindowProperty(x->display, w, atom, 0, 1, False, XA_CARDINAL,
        &type, &format, &count, &remaining, &data);
    if (x11_untrap_errors(x->display, NULL) != 0) {
        if (data != NULL) XFree(data);
        return false;
    }
    bool valid = rc == Success && type == XA_CARDINAL && format == 32 &&
        count == 1 && remaining == 0 && data != NULL;
    if (valid)
        *out = (float)((*(unsigned long *)data & 0xffffffffUL) / 4294967295.0);
    if (data != NULL) XFree(data);
    return valid;
}

static void find_client_window_rec(X11State *x, Window w, Atom wm_state_atom, int *count, Window *found) {
    /* Watch the whole discovered subtree so nested client replacement is visible. */
    XWindowAttributes wa;
    x11_trap_errors(x->display);
    if (x->stats) x->stats->x_round_trips++;
    Status s = XGetWindowAttributes(x->display, w, &wa);
    if (x11_untrap_errors(x->display, NULL) != 0 || !s) return;
    x11_trap_errors(x->display);
    XSelectInput(x->display, w, wa.your_event_mask | PropertyChangeMask |
        StructureNotifyMask | SubstructureNotifyMask);
    x11_untrap_errors(x->display, NULL);

    /* Check if this window has WM_STATE */
    Atom type = None;
    int format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *prop = NULL;
    x11_trap_errors(x->display);
    if (x->stats) x->stats->x_round_trips++;
    int rc = XGetWindowProperty(x->display, w, wm_state_atom, 0, 0, False, AnyPropertyType,
                                &type, &format, &nitems, &bytes_after, &prop);
    if (x11_untrap_errors(x->display, NULL) != 0) {
        if (prop) XFree(prop);
        return;
    }
    if (prop) XFree(prop);
    if (rc == Success && type != None) {
        if (wa.map_state == IsViewable) {
            (*count)++;
            *found = w;
        }
        return;
    }

    Window root_ret, parent_ret;
    Window *children = NULL;
    unsigned int nchildren = 0;
    x11_trap_errors(x->display);
    if (x->stats) x->stats->x_round_trips++;
    Status qtree_status = XQueryTree(x->display, w, &root_ret, &parent_ret, &children, &nchildren);
    if (x11_untrap_errors(x->display, NULL) != 0 || !qtree_status) {
        if (children) XFree(children);
        return;
    }
    if (children) {
        for (unsigned int i = 0; i < nchildren; ++i) {
            find_client_window_rec(x, children[i], wm_state_atom, count, found);
        }
        XFree(children);
    }
}

static Window find_client_window(X11State *x, Window frame, Atom wm_state_atom) {
    int count = 0;
    Window found = None;
    find_client_window_rec(x, frame, wm_state_atom, &count, &found);
    if (count == 1) {
        return found;
    } else if (count > 1) {
        static bool warned = false;
        if (!warned) {
            fprintf(stderr, "slopcom: Multiple viewable clients in frame 0x%lx; using frame/default opacity\n",
                    (unsigned long)frame);
            warned = true;
        }
        return None;
    }
    return None;
}

static float effective_opacity(X11State *x, const WindowState *w) {
    float value;
    if (read_opacity(x, w->xid, x->net_wm_opacity, &value))
        return value;
    if (w->opacity_client != None && w->opacity_client != w->xid &&
        read_opacity(x, w->opacity_client, x->net_wm_opacity, &value))
        return value;
    return 1.0f;
}

static void update_window_opacity(X11State *x, WindowState *w) {
    if (w->opacity_client == None) {
        w->opacity_client = find_client_window(x, w->xid, x->wm_state_atom);
    }
    w->opacity = effective_opacity(x, w);
}

static void update_window_shape(X11State *x, WindowState *w) {
    if (w->shape_rects) {
        XFree(w->shape_rects);
        w->shape_rects = NULL;
        w->num_shape_rects = 0;
        w->shaped = false;
        if (x->stats && x->stats->live_regions > 0) x->stats->live_regions--;
    }

    Bool b_shaped = False, c_shaped = False;
    int bx = 0, by = 0, cx = 0, cy = 0;
    unsigned int bw = 0, bh = 0, cw = 0, ch = 0;

    x11_trap_errors(x->display);
    if (x->stats) x->stats->x_round_trips++;
    Status s = XShapeQueryExtents(x->display, w->xid,
                                 &b_shaped, &bx, &by, &bw, &bh,
                                 &c_shaped, &cx, &cy, &cw, &ch);
    if (x11_untrap_errors(x->display, NULL) == 0 && s && b_shaped) {
        int count = 0, ordering = 0;
        x11_trap_errors(x->display);
        if (x->stats) x->stats->x_round_trips++;
        XRectangle *rects = XShapeGetRectangles(x->display, w->xid, ShapeBounding, &count, &ordering);
        if (x11_untrap_errors(x->display, NULL) == 0 && (count == 0 || rects)) {
            w->shaped = true;
            w->shape_rects = count > 0 ? rects : NULL;
            w->num_shape_rects = count;
            if (count == 0 && rects) XFree(rects);
            if (x->stats && w->shape_rects) x->stats->live_regions++;
            return;
        }
        if (rects) XFree(rects);
    }
    w->shaped = false;
}

WindowState *x11_find_window(X11State *x, Window w) {
    for (size_t i = 0; i < x->num_windows; ++i) {
        if (x->windows[i].xid == w) {
            return &x->windows[i];
        }
    }
    return NULL;
}

static WindowState *find_window_by_opacity_client(X11State *x, Window client) {
    for (size_t i = 0; i < x->num_windows; ++i) {
        if (x->windows[i].opacity_client == client) {
            return &x->windows[i];
        }
    }
    return NULL;
}

static WindowState *find_window_by_damage(X11State *x, Damage dmg) {
    for (size_t i = 0; i < x->num_windows; ++i) {
        if (x->windows[i].damage == dmg) {
            return &x->windows[i];
        }
    }
    return NULL;
}

bool x11_damage_rect(X11State *x, int rx, int ry, int rw, int rh) {
    if (!x || rw <= 0 || rh <= 0) return false;
    DamageRect input = {rx, ry, rw, rh};
    DamageRect screen = {0, 0, x->root_width, x->root_height};
    DamageRect clipped;
    if (!damage_rect_intersect(&input, &screen, &clipped)) return false;
    damage_list_add(&x->client_damage, clipped.x, clipped.y,
                    clipped.width, clipped.height);
    return true;
}

bool x11_damage_window(X11State *x, WindowState *w) {
    if (!w || !w->viewable || w->width <= 0 || w->height <= 0) return false;
    return x11_damage_rect(x, w->x, w->y, w->width, w->height);
}

void x11_update_occlusion(X11State *x) {
    if (!x || x->num_windows == 0) return;

    for (size_t i = 0; i < x->num_windows; ++i) {
        WindowState *w = &x->windows[i];
        if (!w->viewable || w->width <= 0 || w->height <= 0) {
            w->is_occluded = true;
            continue;
        }

        w->is_occluded = false;

        /* Check if any opaque window above w completely covers w */
        for (size_t j = i + 1; j < x->num_windows; ++j) {
            WindowState *top = &x->windows[j];
            if (!top->viewable || top->has_alpha || top->opacity != 1.0f || top->shaped) {
                continue;
            }
            if (top->x <= w->x && top->y <= w->y &&
                (top->x + top->width) >= (w->x + w->width) &&
                (top->y + top->height) >= (w->y + w->height)) {
                w->is_occluded = true;
                break;
            }
        }
    }
}

bool x11_init(X11State *x, const char *display_name) {
    Stats *saved_stats = x->stats;
    bool saved_full_redraw = x->full_redraw;
    bool saved_unredir = x->unredir_fullscreen;
    memset(x, 0, sizeof(*x));
    x->stats = saved_stats;
    x->full_redraw = saved_full_redraw;
    x->unredir_fullscreen = saved_unredir;
    x->fullscreen_recheck = saved_unredir;

    x->display = XOpenDisplay(display_name);
    if (!x->display) {
        fprintf(stderr, "slopcom: Cannot open X display '%s'. Ensure an Xorg session is running and DISPLAY is set.\n",
                display_name ? display_name : (getenv("DISPLAY") ? getenv("DISPLAY") : ":0"));
        return false;
    }

    x11_install_error_handler(x);

    XSetIOErrorHandler(fatal_x_io_error_handler);

    x->screen = DefaultScreen(x->display);
    x->root = RootWindow(x->display, x->screen);
    x->root_width = DisplayWidth(x->display, x->screen);
    x->root_height = DisplayHeight(x->display, x->screen);
    x->root_depth = DefaultDepth(x->display, x->screen);
    x->root_visual_id = XVisualIDFromVisual(DefaultVisual(x->display, x->screen));
    background_init(&x->background, x->display, x->root, x->root_depth);

    /* Query required extensions */
    if (!XCompositeQueryExtension(x->display, &x->composite_event_base, &x->composite_error_base)) {
        fprintf(stderr, "slopcom: XComposite extension not supported on display\n");
        goto fail;
    }
    int comp_major = 0, comp_minor = 0;
    XCompositeQueryVersion(x->display, &comp_major, &comp_minor);
    if (comp_major < 0 || (comp_major == 0 && comp_minor < 3)) {
        fprintf(stderr, "slopcom: XComposite >= 0.3 required, got %d.%d\n", comp_major, comp_minor);
        goto fail;
    }

    if (!XDamageQueryExtension(x->display, &x->damage_event_base, &x->damage_error_base)) {
        fprintf(stderr, "slopcom: XDamage extension not supported on display\n");
        goto fail;
    }
    int dmg_major = 1, dmg_minor = 1;
    XDamageQueryVersion(x->display, &dmg_major, &dmg_minor);

    int dmg_opcode = 0, dummy_ev = 0, dummy_err = 0;
    if (XQueryExtension(x->display, "DAMAGE", &dmg_opcode, &dummy_ev, &dummy_err)) {
        x->damage_opcode = dmg_opcode;
    }

    if (!XFixesQueryExtension(x->display, &x->fixes_event_base, &x->fixes_error_base)) {
        fprintf(stderr, "slopcom: XFixes extension not supported on display\n");
        goto fail;
    }
    int fixes_major = 2, fixes_minor = 0;
    XFixesQueryVersion(x->display, &fixes_major, &fixes_minor);
    if (fixes_major < 2) {
        fprintf(stderr, "slopcom: XFixes >= 2.0 required, got %d.%d\n", fixes_major, fixes_minor);
        goto fail;
    }

    if (!XShapeQueryExtension(x->display, &x->shape_event_base, &x->shape_error_base)) {
        fprintf(stderr, "slopcom: Shape extension not supported on display\n");
        goto fail;
    }
    int shape_major = 0, shape_minor = 0;
    XShapeQueryVersion(x->display, &shape_major, &shape_minor);
    if (shape_major < 1 || (shape_major == 1 && shape_minor < 1)) {
        fprintf(stderr, "slopcom: Shape >= 1.1 required, got %d.%d\n", shape_major, shape_minor);
        goto fail;
    }

    int render_event_base, render_error_base;
    if (!XRenderQueryExtension(x->display, &render_event_base, &render_error_base)) {
        fprintf(stderr, "slopcom: XRender extension not supported on display\n");
        goto fail;
    }

    /* Intern EWMH and ICCCM atoms */
    char net_wm_cm_name[64];
    snprintf(net_wm_cm_name, sizeof(net_wm_cm_name), "_NET_WM_CM_S%d", x->screen);
    x->net_wm_cm_s = XInternAtom(x->display, net_wm_cm_name, False);
    x->manager_atom = XInternAtom(x->display, "MANAGER", False);
    x->net_wm_opacity = XInternAtom(x->display, "_NET_WM_WINDOW_OPACITY", False);
    x->wm_state_atom = XInternAtom(x->display, "WM_STATE", False);
    x->net_wm_bypass_compositor = XInternAtom(x->display, "_NET_WM_BYPASS_COMPOSITOR", False);

    /* Check whether another compositor already owns the selection */
    Window current_owner = XGetSelectionOwner(x->display, x->net_wm_cm_s);
    if (current_owner != None) {
        fprintf(stderr, "slopcom: Another compositing manager (window 0x%lx) already owns screen %d selection (%s).\n",
                (unsigned long)current_owner, x->screen, net_wm_cm_name);
        goto fail;
    }

    /* Create owner window */
    x->owner_win = XCreateSimpleWindow(x->display, x->root, 0, 0, 1, 1, 0, 0, 0);
    if (x->owner_win == None) {
        fprintf(stderr, "slopcom: Failed to create selection owner window\n");
        goto fail;
    }

    /* Grab server briefly to safely acquire selection without races */
    XGrabServer(x->display);
    current_owner = XGetSelectionOwner(x->display, x->net_wm_cm_s);
    if (current_owner != None) {
        XUngrabServer(x->display);
        fprintf(stderr, "slopcom: Another compositing manager acquired screen %d selection during startup\n", x->screen);
        goto fail;
    }

    /* Obtain a server timestamp safely using a property notify on owner_win */
    XSelectInput(x->display, x->owner_win, PropertyChangeMask);
    Atom timestamp_atom = XInternAtom(x->display, "_SLOPCOM_TIMESTAMP", False);
    unsigned char dummy_prop = 1;
    XChangeProperty(x->display, x->owner_win, timestamp_atom, timestamp_atom, 8,
                    PropModeReplace, &dummy_prop, 1);
    XEvent time_ev;
    XWindowEvent(x->display, x->owner_win, PropertyChangeMask, &time_ev);
    Time timestamp = time_ev.xproperty.time;

    /* Acquire selection */
    XSetSelectionOwner(x->display, x->net_wm_cm_s, x->owner_win, timestamp);
    if (XGetSelectionOwner(x->display, x->net_wm_cm_s) != x->owner_win) {
        XUngrabServer(x->display);
        fprintf(stderr, "slopcom: Failed to acquire selection %s\n", net_wm_cm_name);
        goto fail;
    }
    x->selection_acquired = true;

    /* Announce MANAGER via ClientMessage on root window */
    XClientMessageEvent cme;
    memset(&cme, 0, sizeof(cme));
    cme.type = ClientMessage;
    cme.window = x->root;
    cme.message_type = x->manager_atom;
    cme.format = 32;
    cme.data.l[0] = (long)timestamp;
    cme.data.l[1] = (long)x->net_wm_cm_s;
    cme.data.l[2] = (long)x->owner_win;
    cme.data.l[3] = 0;
    cme.data.l[4] = 0;
    XSendEvent(x->display, x->root, False, StructureNotifyMask, (XEvent *)&cme);

    XUngrabServer(x->display);

    /* Subscribe to root structure and property change events without clobbering existing masks */
    XWindowAttributes root_attrs;
    long root_event_mask = SubstructureNotifyMask | StructureNotifyMask | PropertyChangeMask;
    if (XGetWindowAttributes(x->display, x->root, &root_attrs)) {
        root_event_mask |= root_attrs.your_event_mask;
    }
    XSelectInput(x->display, x->root, root_event_mask);

    /* Acquire manual redirection with checked BadAccess trap */
    x11_trap_errors(x->display);
    XCompositeRedirectSubwindows(x->display, x->root, CompositeRedirectManual);
    unsigned char trap_err = 0;
    if (x11_untrap_errors(x->display, &trap_err) != 0) {
        fprintf(stderr, "slopcom: Failed to acquire manual redirection on root window (X error %d%s)\n",
                trap_err, trap_err == BadAccess ? " - BadAccess" : "");
        goto fail;
    }
    x->redirected = true;

    /* Acquire Composite Overlay Window */
    x->overlay = XCompositeGetOverlayWindow(x->display, x->root);
    if (x->overlay == None) {
        fprintf(stderr, "slopcom: Failed to get Composite Overlay Window\n");
        goto fail;
    }
    x->overlay_acquired = true;

    /* Give overlay empty input region using XFixes/Shape so mouse/keyboard pass through */
    XserverRegion empty_region = XFixesCreateRegion(x->display, NULL, 0);
    XFixesSetWindowShapeRegion(x->display, x->overlay, ShapeInput, 0, 0, empty_region);
    XFixesDestroyRegion(x->display, empty_region);

    /* Select ExposureMask on overlay so lost contents trigger redraw */
    XSelectInput(x->display, x->overlay, ExposureMask);

    /* Retrieve overlay visual */
    XWindowAttributes overlay_attrs;
    if (!XGetWindowAttributes(x->display, x->overlay, &overlay_attrs)) {
        fprintf(stderr, "slopcom: Failed to get overlay window attributes\n");
        goto fail;
    }
    x->overlay_visual_id = XVisualIDFromVisual(overlay_attrs.visual);

    /* Initialize scene damage tracking and history */
    x->scene_damage = XFixesCreateRegion(x->display, NULL, 0);
    if (x->stats) x->stats->live_regions++;
    x->history_count = 0;
    x->history_head = 0;
    x->needs_full_repaint = true;
    x11_damage_rect(x, 0, 0, x->root_width, x->root_height);

    x->pending_reconcile = true;
    x->pending_changes = CHANGE_HIERARCHY;
    return true;

fail:
    x11_destroy(x, NULL);
    return false;
}

void x11_invalidate_full_scene(X11State *x) {
    if (!x) return;
    x->history_count = x->history_head = 0;
    for (int i = 0; i < DAMAGE_HISTORY_CAP; i++) damage_list_clear(&x->client_history[i]);
    damage_list_clear(&x->client_damage);
    x->needs_full_repaint = true;
    x11_damage_rect(x, 0, 0, x->root_width, x->root_height);
}

static void release_window_storage(Display *dpy, EGLState *e, WindowState *w, Stats *stats) {
    if (e && w->imported.image != EGL_NO_IMAGE_KHR) {
        egl_release_import(e, &w->imported);
    }
    w->imported.image = EGL_NO_IMAGE_KHR;
    w->imported.texture = 0;

    if (w->damage != None) {
        x11_trap_errors(dpy);
        XDamageDestroy(dpy, w->damage);
        x11_untrap_errors(dpy, NULL);
        w->damage = None;
        if (stats && stats->live_damage > 0) stats->live_damage--;
    }
    if (w->pixmap != None) {
        x11_trap_errors(dpy);
        XFreePixmap(dpy, w->pixmap);
        x11_untrap_errors(dpy, NULL);
        w->pixmap = None;
        if (stats && stats->live_pixmaps > 0) stats->live_pixmaps--;
    }
}

static void release_window_resources(Display *dpy, EGLState *e, WindowState *w, Stats *stats) {
    release_window_storage(dpy, e, w, stats);
    if (w->shape_rects) {
        XFree(w->shape_rects);
        w->shape_rects = NULL;
        w->num_shape_rects = 0;
        w->shaped = false;
        if (stats && stats->live_regions > 0) stats->live_regions--;
    }
}

static bool query_window_viewability(X11State *x, Window window, bool *viewable) {
    XWindowAttributes wa;
    XErrorTrap trap;
    x11_trap_errors_scoped(x->display, &trap);
    if (x->stats) x->stats->x_round_trips++;
    Status status = XGetWindowAttributes(x->display, window, &wa);
    bool failed = x11_untrap_errors_scoped(&trap) != 0;
    if (!failed && status) {
        *viewable = wa.map_state == IsViewable;
        return true;
    }
    if (failed && trap.error_code == BadWindow) {
        *viewable = false;
        return true;
    }
    return false;
}

static bool x11_reconcile_internal(X11State *x, EGLState *e, bool require_complete_scene) {
    bool strict_scene = require_complete_scene ||
        x->fullscreen_mode == FULLSCREEN_RESTORE_HIDDEN ||
        x->fullscreen_mode == FULLSCREEN_RESTORE_VISIBLE ||
        (x->unredir_fullscreen && x->fullscreen_recheck);
    Window root_ret, parent_ret;
    Window *children = NULL;
    unsigned int nchildren = 0;

    x11_trap_errors(x->display);
    if (x->stats) x->stats->x_round_trips++;
    Status status = XQueryTree(x->display, x->root, &root_ret, &parent_ret, &children, &nchildren);
    if (x11_untrap_errors(x->display, NULL) != 0 || !status) {
        if (children) XFree(children);
        fprintf(stderr, "slopcom: Cannot query root children during scene reconciliation\n");
        return false;
    }

    /* Batch top-level child attributes and geometry queries via borrowed XCB connection */
    WindowProbe *probes = NULL;
    bool probe_scene_incomplete = false;
    if (nchildren > 0) {
        xcb_connection_t *connection = XGetXCBConnection(x->display);
        if (!connection) {
            fprintf(stderr, "slopcom: Failed to borrow XCB connection from Xlib Display\n");
            if (children) XFree(children);
            return false;
        }
        if (xcb_connection_has_error(connection)) {
            if (children) XFree(children);
            fatal_x_io_error_handler(x->display);
        }

        probes = calloc(nchildren, sizeof(WindowProbe));
        if (!probes) {
            if (children) XFree(children);
            return false;
        }

        unsigned int eligible_children = 0;
        for (unsigned int i = 0; i < nchildren; ++i) {
            probes[i].xid = children[i];
            if (children[i] != x->owner_win && children[i] != x->overlay) {
                eligible_children++;
            }
        }

        if (eligible_children > 0) {
            XFlush(x->display);
            for (unsigned int i = 0; i < nchildren; ++i) {
                if (probes[i].xid == x->owner_win || probes[i].xid == x->overlay) {
                    continue;
                }
                if (x->stats) x->stats->x_round_trips++;
                probes[i].attributes_cookie = xcb_get_window_attributes(connection, probes[i].xid);
                if (x->stats) x->stats->x_round_trips++;
                probes[i].geometry_cookie = xcb_get_geometry(connection, probes[i].xid);
                probes[i].queued = true;
            }
            if (xcb_flush(connection) <= 0) {
                free(probes);
                if (children) XFree(children);
                fatal_x_io_error_handler(x->display);
            }
        }

        for (unsigned int i = 0; i < nchildren; ++i) {
            if (!probes[i].queued) continue;

            xcb_generic_error_t *attr_err = NULL;
            xcb_generic_error_t *geom_err = NULL;
            xcb_get_window_attributes_reply_t *attr_reply =
                xcb_get_window_attributes_reply(connection, probes[i].attributes_cookie, &attr_err);
            xcb_get_geometry_reply_t *geom_reply =
                xcb_get_geometry_reply(connection, probes[i].geometry_cookie, &geom_err);

            log_window_query_error(probes[i].xid, "GetWindowAttributes", attr_err, BadWindow);
            log_window_query_error(probes[i].xid, "GetGeometry", geom_err, BadDrawable);
            if (attr_err || geom_err || !attr_reply || !geom_reply) {
                bool vanished =
                    (attr_err && attr_err->error_code == BadWindow) ||
                    (geom_err && geom_err->error_code == BadDrawable);
                bool unexpected_error =
                    (attr_err && attr_err->error_code != BadWindow) ||
                    (geom_err && geom_err->error_code != BadDrawable);
                bool unexplained_missing_reply =
                    (!attr_reply && !attr_err) || (!geom_reply && !geom_err);
                if (strict_scene && (!vanished || unexpected_error || unexplained_missing_reply)) {
                    probe_scene_incomplete = true;
                }
                free(attr_err);
                free(geom_err);
                free(attr_reply);
                free(geom_reply);
                continue;
            }

            XWindowAttributes *wa = &probes[i].attributes;
            wa->class = attr_reply->_class;
            wa->map_state = attr_reply->map_state;
            wa->your_event_mask = attr_reply->your_event_mask;
            wa->override_redirect = attr_reply->override_redirect;

            wa->root = geom_reply->root;
            wa->depth = geom_reply->depth;
            wa->x = geom_reply->x;
            wa->y = geom_reply->y;
            wa->width = geom_reply->width;
            wa->height = geom_reply->height;
            wa->border_width = geom_reply->border_width;

            if (wa->class == InputOutput && wa->map_state == IsViewable) {
                wa->visual = lookup_visual_by_id(x->display, x->screen, attr_reply->visual);
                if (!wa->visual) {
                    fprintf(stderr, "slopcom: Visual 0x%x for window 0x%lx not found in screen depths\n",
                            attr_reply->visual, (unsigned long)probes[i].xid);
                    if (strict_scene) probe_scene_incomplete = true;
                } else {
                    probes[i].valid = true;
                }
            }

            free(attr_reply);
            free(geom_reply);
        }

        if (xcb_connection_has_error(connection)) {
            free(probes);
            if (children) XFree(children);
            fatal_x_io_error_handler(x->display);
        }
    }

    if (probe_scene_incomplete) {
        free(probes);
        if (children) XFree(children);
        fprintf(stderr, "slopcom: Incomplete root-child probes during fullscreen restoration\n");
        return false;
    }

    /* Allocate new windows array */
    bool metadata_only = (x->fullscreen_mode == FULLSCREEN_BYPASSED);
    WindowState *new_windows = calloc(nchildren, sizeof(WindowState));
    if (!new_windows && nchildren > 0) {
        free(probes);
        if (children) XFree(children);
        return false;
    }
    size_t new_count = 0;
    bool reconciliation_failed = false;

    Window *old_xids = NULL;
    if (x->num_windows > 0) {
        old_xids = malloc(x->num_windows * sizeof(Window));
        if (old_xids) {
            for (size_t i = 0; i < x->num_windows; ++i) {
                old_xids[i] = x->windows[i].xid;
            }
        }
    }
    bool scene_changed = false;

    for (unsigned int i = 0; i < nchildren; ++i) {
        Window w = children[i];
        if (w == x->owner_win || w == x->overlay) {
            continue;
        }

        if (!probes[i].valid) {
            continue;
        }
        XWindowAttributes wa = probes[i].attributes;

        WindowState *existing = x11_find_window(x, w);
        WindowState *nw = &new_windows[new_count++];
        memset(nw, 0, sizeof(*nw));
        nw->imported.image = EGL_NO_IMAGE_KHR;

        int old_x = 0, old_y = 0, old_w = 0, old_h = 0, old_bw = 0;
        float old_op = 1.0f;
        bool old_shaped = false;
        Pixmap old_pixmap = None;
        bool was_existing = (existing != NULL);

        if (existing) {
            old_x = existing->x;
            old_y = existing->y;
            old_w = existing->width;
            old_h = existing->height;
            old_bw = existing->border_width;
            old_op = existing->opacity;
            old_shaped = existing->shaped;
            old_pixmap = existing->pixmap;

            *nw = *existing;
            /* Invalidate in old list so it won't be freed if reconcile succeeds */
            memset(existing, 0, sizeof(*existing));
            existing->imported.image = EGL_NO_IMAGE_KHR;

            /* Damage can be retired after an asynchronous BadDamage while the
             * window and its imported pixmap remain valid. Recreate only the
             * Damage resource; do not force a pixmap reimport. */
            if (!metadata_only && nw->damage == None) {
                bool vanished = false;
                nw->damage = create_damage_resource(x, w, &vanished);
                if (nw->damage != None && x->stats) x->stats->live_damage++;
                if (vanished) {
                    if (x11_damage_rect(x, old_x, old_y, old_w, old_h))
                        scene_changed = true;
                    release_window_resources(x->display, e, nw, x->stats);
                    new_count--;
                    continue;
                }
                if (nw->damage == None) {
                    fprintf(stderr, "slopcom: Cannot create Damage tracking for viewable window 0x%lx; stopping\n",
                            (unsigned long)w);
                    reconciliation_failed = true;
                    break;
                }
            }

            if (x->pending_changes & CHANGE_HIERARCHY) {
                nw->opacity_client = None;
                nw->opacity_dirty = true;
            }
            if (nw->opacity_dirty) {
                update_window_opacity(x, nw);
                nw->opacity_dirty = false;
            }
        } else {
            nw->xid = w;
            nw->x = wa.x;
            nw->y = wa.y;
            nw->width = wa.width;
            nw->height = wa.height;
            nw->border_width = wa.border_width;
            nw->depth = wa.depth;
            nw->visual_id = XVisualIDFromVisual(wa.visual);
            nw->viewable = true;
            nw->has_alpha = check_visual_has_alpha(x->display, wa.visual);
            nw->storage_dirty = !metadata_only;

            x11_trap_errors(x->display);
            XSelectInput(x->display, w, wa.your_event_mask | PropertyChangeMask |
                StructureNotifyMask | SubstructureNotifyMask);
            XShapeSelectInput(x->display, w, ShapeNotifyMask);
            x11_untrap_errors(x->display, NULL);

            if (!metadata_only) {
                bool vanished = false;
                nw->damage = create_damage_resource(x, w, &vanished);
                if (nw->damage != None && x->stats) x->stats->live_damage++;
                if (vanished) {
                    new_count--;
                    continue;
                }
                if (nw->damage == None) {
                    fprintf(stderr, "slopcom: Cannot create Damage tracking for viewable window 0x%lx; stopping\n",
                            (unsigned long)w);
                    reconciliation_failed = true;
                    break;
                }
            } else {
                nw->damage = None;
            }

            nw->opacity_dirty = true;
            update_window_opacity(x, nw);
            nw->opacity_dirty = false;
            update_window_shape(x, nw);
        }

        nw->x = wa.x;
        nw->y = wa.y;
        nw->border_width = wa.border_width;
        nw->viewable = (wa.map_state == IsViewable);
        nw->override_redirect = (wa.override_redirect != 0);

        if (metadata_only) {
            nw->width = wa.width + 2 * wa.border_width;
            nw->height = wa.height + 2 * wa.border_width;
            nw->depth = wa.depth;
            nw->storage_dirty = false;
        } else if (nw->storage_dirty || nw->imported.image == EGL_NO_IMAGE_KHR) {
            bool is_reimport = (nw->imported.image != EGL_NO_IMAGE_KHR);
            if (nw->imported.image != EGL_NO_IMAGE_KHR) {
                egl_release_import(e, &nw->imported);
            }
            if (nw->pixmap != None) {
                x11_trap_errors(x->display);
                XFreePixmap(x->display, nw->pixmap);
                x11_untrap_errors(x->display, NULL);
                nw->pixmap = None;
                if (x->stats && x->stats->live_pixmaps > 0) x->stats->live_pixmaps--;
            }

            XErrorTrap capture_trap;
            x11_trap_errors_scoped(x->display, &capture_trap);
            nw->pixmap = XCompositeNameWindowPixmap(x->display, nw->xid);
            if (x->stats && nw->pixmap != None) x->stats->live_pixmaps++;
            Window geom_root;
            int gx = 0, gy = 0;
            unsigned int gw = 0, gh = 0, gborder = 0, gdepth = 0;
            Status geom_status = False;
            if (nw->pixmap != None) {
                if (x->stats) x->stats->x_round_trips++;
                geom_status = XGetGeometry(x->display, nw->pixmap, &geom_root,
                                           &gx, &gy, &gw, &gh, &gborder, &gdepth);
            }
            bool capture_failed = x11_untrap_errors_scoped(&capture_trap) != 0 || !geom_status;
            if (capture_failed) {
                bool still_viewable = false;
                bool viewability_known = query_window_viewability(x, nw->xid, &still_viewable);
                if (!viewability_known || still_viewable) {
                    fprintf(stderr,
                            "slopcom: Cannot capture viewable window 0x%lx; stopping\n",
                            (unsigned long)nw->xid);
                    reconciliation_failed = true;
                    break;
                }
                release_window_resources(x->display, e, nw, x->stats);
                new_count--;
                continue;
            }

            nw->width = (int)gw;
            nw->height = (int)gh;
            nw->depth = (int)gdepth;

            XSync(x->display, False);
            if (x->stats) x->stats->x_round_trips++;

            if (e && !egl_import_pixmap(e, nw->pixmap, &nw->imported)) {
                /* Check if window still exists */
                XWindowAttributes check_wa;
                x11_trap_errors(x->display);
                if (x->stats) x->stats->x_round_trips++;
                Status check_status = XGetWindowAttributes(x->display, nw->xid, &check_wa);
                if (x11_untrap_errors(x->display, NULL) == 0 && check_status) {
                    /* Window exists but failed to import -> fatal error */
                    fprintf(stderr, "slopcom: Cannot capture/import viewable window 0x%lx; stopping\n",
                            (unsigned long)nw->xid);
                    reconciliation_failed = true;
                    break;
                } else {
                    /* Window vanished during import -> remove gracefully */
                    release_window_resources(x->display, e, nw, x->stats);
                    new_count--;
                    continue;
                }
            } else {
                nw->storage_dirty = false;
                if (x->stats) {
                    if (is_reimport) {
                        x->stats->reimports++;
                    } else {
                        x->stats->imports++;
                    }
                }
            }
        }

        if (!metadata_only) {
            if (!was_existing) {
                if (x11_damage_window(x, nw)) {
                    scene_changed = true;
                }
            } else {
                if (nw->x != old_x || nw->y != old_y || nw->width != old_w ||
                    nw->height != old_h || nw->border_width != old_bw ||
                    nw->opacity != old_op || nw->shaped != old_shaped ||
                    nw->pixmap != old_pixmap) {
                    bool d1 = x11_damage_rect(x, old_x, old_y, old_w, old_h);
                    bool d2 = x11_damage_window(x, nw);
                    if (d1 || d2) {
                        scene_changed = true;
                    }
                }
            }
        }
    }

    if (children) {
        XFree(children);
    }
    if (probes) {
        free(probes);
    }

    if (old_xids) {
        if (!metadata_only) {
            for (size_t i = 0; i < new_count && i < x->num_windows; ++i) {
                if (new_windows[i].xid != old_xids[i]) {
                    if (x11_damage_rect(x, 0, 0, x->root_width, x->root_height)) {
                        scene_changed = true;
                    }
                    break;
                }
            }
        }
        free(old_xids);
    }

    if (reconciliation_failed) {
        for (size_t i = 0; i < new_count; ++i) {
            release_window_resources(x->display, e, &new_windows[i], x->stats);
        }
        free(new_windows);
        return false;
    }

    if (x->windows) {
        for (size_t i = 0; i < x->num_windows; ++i) {
            if (!metadata_only && x->windows[i].xid != None && x->windows[i].viewable) {
                if (x11_damage_window(x, &x->windows[i])) {
                    scene_changed = true;
                }
            }
            release_window_resources(x->display, e, &x->windows[i], x->stats);
        }
        free(x->windows);
    }

    x->windows = new_windows;
    x->num_windows = new_count;
    x->cap_windows = nchildren;
    x->pending_changes = CHANGE_NONE;
    x11_update_occlusion(x);
    if (!metadata_only && scene_changed) {
        x->scene_dirty = true;
    }
    return true;
}

bool x11_reconcile(X11State *x, EGLState *e) {
    return x11_reconcile_internal(x, e, false);
}

static bool check_raw_opacity_full(Display *dpy, Window w, Atom opacity_atom, Stats *stats) {
    Atom type = None;
    int format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *prop = NULL;
    x11_trap_errors(dpy);
    if (stats) stats->x_round_trips++;
    int rc = XGetWindowProperty(dpy, w, opacity_atom, 0, 1, False, XA_CARDINAL,
                                &type, &format, &nitems, &bytes_after, &prop);
    if (x11_untrap_errors(dpy, NULL) != 0) {
        if (prop) XFree(prop);
        return false;
    }
    if (rc != Success) {
        if (prop) XFree(prop);
        return false;
    }
    if (type == None) {
        return true; /* Absent property -> default fully opaque */
    }
    bool ok = (type == XA_CARDINAL && format == 32 && nitems == 1 && bytes_after == 0 && prop != NULL);
    if (ok) {
        unsigned long val = *(unsigned long *)prop;
        if ((val & 0xffffffffUL) != 0xffffffffUL) {
            ok = false;
        }
    }
    if (prop) XFree(prop);
    return ok;
}

static bool check_bypass_hint(Display *dpy, Window w, Atom bypass_atom, Stats *stats) {
    Atom type = None;
    int format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *prop = NULL;
    x11_trap_errors(dpy);
    if (stats) stats->x_round_trips++;
    int rc = XGetWindowProperty(dpy, w, bypass_atom, 0, 1, False, XA_CARDINAL,
                                &type, &format, &nitems, &bytes_after, &prop);
    if (x11_untrap_errors(dpy, NULL) != 0) {
        if (prop) XFree(prop);
        return false;
    }
    if (rc != Success) {
        if (prop) XFree(prop);
        return false;
    }
    if (type == None) {
        return true; /* Absent hint -> no preference */
    }
    if (type != XA_CARDINAL || format != 32 || nitems != 1 || bytes_after != 0 || !prop) {
        if (prop) XFree(prop);
        return false; /* Malformed hint -> decline */
    }
    unsigned long val = (*(unsigned long *)prop) & 0xffffffffUL;
    XFree(prop);
    if (val == 2) {
        return false; /* Veto bypass */
    }
    return true; /* 0, 1, or reserved -> allowed */
}

static WindowState *find_fullscreen_candidate(X11State *x, Window *client_out) {
    if (!x || !x->unredir_fullscreen || x->num_windows == 0) return NULL;

    /* Stacking order in x->windows is bottom-to-top. Walk from top to bottom. */
    WindowState *cand = NULL;
    for (int i = (int)x->num_windows - 1; i >= 0; --i) {
        WindowState *w = &x->windows[i];
        if (!w->viewable) continue;
        int left = w->x;
        int top = w->y;
        int right = w->x + w->width;
        int bottom = w->y + w->height;
        if (w->width <= 0 || w->height <= 0 ||
            right <= 0 || left >= x->root_width ||
            bottom <= 0 || top >= x->root_height) {
            continue; /* Outer bounds do not intersect root */
        }
        cand = w;
        break; /* The first remaining window is the ONLY possible candidate. */
    }

    if (!cand) return NULL;

    /* 1. Managed InputOutput root child with exactly one live client */
    if (cand->override_redirect) return NULL;
    Window client = find_client_window(x, cand->xid, x->wm_state_atom);
    if (client == None) return NULL;

    /* Keep candidate-client shape changes observable even when shape eligibility
     * rejects bypass. Shape selection is separate from the core event mask. */
    if (client != cand->xid) {
        x11_trap_errors(x->display);
        XShapeSelectInput(x->display, client, ShapeNotifyMask);
        if (x11_untrap_errors(x->display, NULL) != 0) return NULL;
    }

    /* 2. Both frame and client have zero border width and cover (0, 0, root_width, root_height) */
    if (cand->border_width != 0 || cand->x != 0 || cand->y != 0 ||
        cand->width != x->root_width || cand->height != x->root_height) {
        return NULL;
    }

    XWindowAttributes cwa;
    if (client != cand->xid) {
        x11_trap_errors(x->display);
        if (x->stats) x->stats->x_round_trips++;
        Status cs = XGetWindowAttributes(x->display, client, &cwa);
        if (x11_untrap_errors(x->display, NULL) != 0 || !cs) return NULL;
        if (cwa.border_width != 0 || cwa.width != x->root_width || cwa.height != x->root_height) {
            return NULL;
        }
        int rx = 0, ry = 0;
        Window child_ret = None;
        x11_trap_errors(x->display);
        if (x->stats) x->stats->x_round_trips++;
        Bool tr = XTranslateCoordinates(x->display, client, x->root, 0, 0, &rx, &ry, &child_ret);
        if (x11_untrap_errors(x->display, NULL) != 0 || !tr) return NULL;
        if (rx != 0 || ry != 0) return NULL;
    }

    /* 3. Both frame and client use visuals without alpha and have rectangular, unshaped bounding regions */
    if (cand->has_alpha || cand->shaped) return NULL;
    if (client != cand->xid) {
        if (check_visual_has_alpha(x->display, cwa.visual)) return NULL;
        Bool b_shaped = False, c_shaped = False;
        int bx = 0, by = 0, cx = 0, cy = 0;
        unsigned int bw = 0, bh = 0, cw = 0, ch = 0;
        x11_trap_errors(x->display);
        if (x->stats) x->stats->x_round_trips++;
        Status ss = XShapeQueryExtents(x->display, client,
                                      &b_shaped, &bx, &by, &bw, &bh,
                                      &c_shaped, &cx, &cy, &cw, &ch);
        if (x11_untrap_errors(x->display, NULL) != 0 || !ss || b_shaped) return NULL;
    }

    /* 4. Frame and client opacity properties are absent or valid CARDINAL 32 == UINT32_MAX */
    if (!check_raw_opacity_full(x->display, cand->xid, x->net_wm_opacity, x->stats)) return NULL;
    if (client != cand->xid) {
        if (!check_raw_opacity_full(x->display, client, x->net_wm_opacity, x->stats)) return NULL;
    }

    /* 5. Neither frame nor client has _NET_WM_BYPASS_COMPOSITOR = 2 */
    if (!check_bypass_hint(x->display, cand->xid, x->net_wm_bypass_compositor, x->stats)) return NULL;
    if (client != cand->xid) {
        if (!check_bypass_hint(x->display, client, x->net_wm_bypass_compositor, x->stats)) return NULL;
    }

    if (client_out) *client_out = client;
    return cand;
}

static bool recover_failed_fullscreen_entry(X11State *x, EGLState *e, bool *dirty) {
    /* Entry released storage before the checked server transition. Rebuild it
     * synchronously so the caller cannot render this iteration without clients. */
    x->pending_reconcile = false;
    x->pending_changes = CHANGE_HIERARCHY;
    if (!x11_reconcile_internal(x, e, true) || x->pending_changes != CHANGE_NONE) {
        fprintf(stderr, "slopcom: Failed to restore window storage after fullscreen entry failure\n");
        return false;
    }
    x->pending_reconcile = false;
    x11_invalidate_full_scene(x);
    if (e) e->presentation_damage_count = 0;
    if (dirty) *dirty = true;
    return true;
}

static bool x11_enter_fullscreen_bypass(X11State *x, EGLState *e, Window cand_frame, Window cand_client, bool *dirty) {
    x11_finish_damage_batch(x);

    /* Release per-window storage while EGL is current */
    for (size_t i = 0; i < x->num_windows; ++i) {
        release_window_storage(x->display, e, &x->windows[i], x->stats);
    }

    /* Check root unredirection */
    XErrorTrap trap;
    x11_trap_errors_scoped(x->display, &trap);
    XCompositeUnredirectSubwindows(x->display, x->root, CompositeRedirectManual);
    if (x11_untrap_errors_scoped(&trap) != 0) {
        fprintf(stderr, "slopcom: Failed to unredirect root subwindows for fullscreen bypass\n");
        x->unredir_fullscreen = false;
        return recover_failed_fullscreen_entry(x, e, dirty);
    }
    x->redirected = false;

    /* Hide overlay by setting ShapeBounding to empty region */
    XserverRegion empty = XFixesCreateRegion(x->display, NULL, 0);
    x11_trap_errors_scoped(x->display, &trap);
    XFixesSetWindowShapeRegion(x->display, x->overlay, ShapeBounding, 0, 0, empty);
    XFixesDestroyRegion(x->display, empty);
    if (x11_untrap_errors_scoped(&trap) != 0) {
        fprintf(stderr, "slopcom: Failed to hide overlay for fullscreen bypass\n");
        x->unredir_fullscreen = false;
        x11_trap_errors_scoped(x->display, &trap);
        XCompositeRedirectSubwindows(x->display, x->root, CompositeRedirectManual);
        if (x11_untrap_errors_scoped(&trap) != 0) {
            fprintf(stderr, "slopcom: Failed to reacquire redirection after overlay hide failure\n");
            return false;
        }
        x->redirected = true;
        return recover_failed_fullscreen_entry(x, e, dirty);
    }
    x->overlay_hidden = true;

    x->fullscreen_mode = FULLSCREEN_BYPASSED;
    x->bypass_frame = cand_frame;
    x->bypass_client = cand_client;

    /* Discard old damage and history */
    x->history_count = x->history_head = 0;
    for (int i = 0; i < DAMAGE_HISTORY_CAP; i++) damage_list_clear(&x->client_history[i]);
    damage_list_clear(&x->client_damage);
    if (x->scene_damage != None) XFixesSetRegion(x->display, x->scene_damage, NULL, 0);
    if (e) e->presentation_damage_count = 0;
    x->needs_full_repaint = false;
    x->scene_dirty = false;
    if (dirty) *dirty = false;

    return true;
}

static bool x11_enter_fullscreen_restoration(X11State *x, EGLState *e, bool *dirty) {
    XErrorTrap trap;
    x11_trap_errors_scoped(x->display, &trap);
    XCompositeRedirectSubwindows(x->display, x->root, CompositeRedirectManual);
    if (x11_untrap_errors_scoped(&trap) != 0) {
        fprintf(stderr, "slopcom: Failed to reacquire root redirection during restoration\n");
        return false;
    }
    x->redirected = true;
    x->fullscreen_mode = FULLSCREEN_RESTORE_HIDDEN;

    /* Handle root size change if pending */
    if (x->root_size_changed) {
        x->root_size_changed = false;
        XWindowAttributes wa;
        XErrorTrap size_trap;
        x11_trap_errors_scoped(x->display, &size_trap);
        if (x->stats) x->stats->x_round_trips++;
        Status size_status = XGetWindowAttributes(x->display, x->root, &wa);
        if (x11_untrap_errors_scoped(&size_trap) != 0 || !size_status) {
            fprintf(stderr, "slopcom: Cannot query root dimensions during fullscreen restoration\n");
            return false;
        }
        x->root_width = wa.width;
        x->root_height = wa.height;
        glViewport(0, 0, wa.width, wa.height);
        x->background.refresh_required = true;
    }

    /* Handle background refresh if pending */
    if (x->background.refresh_required) {
        background_refresh(&x->background, e);
        x->background.refresh_required = false;
    }

    /* Mark all viewable windows storage_dirty so normal reconcile rebuilds storage */
    for (size_t i = 0; i < x->num_windows; ++i) {
        if (x->windows[i].viewable) {
            x->windows[i].storage_dirty = true;
        }
    }

    /* Reconcile scene with full storage acquisition */
    if (!x11_reconcile_internal(x, e, true)) {
        fprintf(stderr, "slopcom: Failed to rebuild scene during fullscreen restoration\n");
        return false;
    }

    x11_invalidate_full_scene(x);
    if (e) e->presentation_damage_count = 0;
    if (dirty) *dirty = true;

    return true;
}

bool x11_reconcile_and_advance_fullscreen(X11State *x, EGLState *e, bool *dirty) {
    if (!x->unredir_fullscreen) {
        return true;
    }

    if (x->fullscreen_mode == FULLSCREEN_RESTORE_HIDDEN ||
        x->fullscreen_mode == FULLSCREEN_RESTORE_VISIBLE) {
        /* No eligibility transition may interrupt either restoration phase */
        return true;
    }

    if (x->fullscreen_mode == FULLSCREEN_COMPOSITED) {
        if (!x->fullscreen_recheck) {
            return true;
        }
        if (XPending(x->display) > 0 || x->pending_reconcile ||
            x->root_size_changed || x->background.refresh_required) {
            return true; /* Keep the invalidation until its scene work is consumed. */
        }

        /* Consume the invalidation being evaluated. If X work appears while
         * the synchronous checks run, put it back and defer the transition. */
        x->fullscreen_recheck = false;
        Window cand_client = None;
        WindowState *cand = find_fullscreen_candidate(x, &cand_client);
        if (XPending(x->display) > 0 || x->pending_reconcile ||
            x->root_size_changed || x->background.refresh_required ||
            x->fullscreen_recheck) {
            x->fullscreen_recheck = true;
            return true;
        }
        if (cand != NULL) {
            return x11_enter_fullscreen_bypass(x, e, cand->xid, cand_client, dirty);
        }
        return true;
    }

    if (x->fullscreen_mode == FULLSCREEN_BYPASSED) {
        if (x->root_size_changed || x->background.refresh_required) {
            return x11_enter_fullscreen_restoration(x, e, dirty);
        }

        if (!x->fullscreen_recheck) {
            /* Still bypassed: clear paint-only flags without querying policy. */
            x->scene_dirty = false;
            x->needs_full_repaint = false;
            damage_list_clear(&x->client_damage);
            if (dirty) *dirty = false;
            return true;
        }

        if (XPending(x->display) > 0 || x->pending_reconcile) {
            return true;
        }

        x->fullscreen_recheck = false;
        Window cand_client = None;
        WindowState *cand = find_fullscreen_candidate(x, &cand_client);
        if (XPending(x->display) > 0 || x->pending_reconcile ||
            x->root_size_changed || x->background.refresh_required ||
            x->fullscreen_recheck) {
            x->fullscreen_recheck = true;
            return true;
        }
        if (!cand || cand->xid != x->bypass_frame || cand_client != x->bypass_client) {
            return x11_enter_fullscreen_restoration(x, e, dirty);
        }

        x->scene_dirty = false;
        x->needs_full_repaint = false;
        damage_list_clear(&x->client_damage);
        if (dirty) *dirty = false;
        return true;
    }

    return true;
}

bool x11_finish_fullscreen_presentation(X11State *x, EGLState *e, bool *dirty) {
    if (x->fullscreen_mode == FULLSCREEN_RESTORE_HIDDEN) {
        XErrorTrap trap;
        x11_trap_errors_scoped(x->display, &trap);
        XFixesSetWindowShapeRegion(x->display, x->overlay, ShapeBounding, 0, 0, None);
        if (x11_untrap_errors_scoped(&trap) != 0) {
            fprintf(stderr, "slopcom: Failed to restore overlay bounding shape during restoration\n");
            return false;
        }
        x->overlay_hidden = false;
        x->fullscreen_mode = FULLSCREEN_RESTORE_VISIBLE;

        x11_invalidate_full_scene(x);
        if (e) e->presentation_damage_count = 0;
        if (dirty) *dirty = true;
        return true;
    }

    if (x->fullscreen_mode == FULLSCREEN_RESTORE_VISIBLE) {
        x->fullscreen_mode = FULLSCREEN_COMPOSITED;
        x->bypass_frame = None;
        x->bypass_client = None;
        return true;
    }

    return true;
}

void x11_handle_event(X11State *x, XEvent *ev, bool *dirty, bool *stop_requested) {
    if (x->stats) x->stats->x_events++;

    if (ev->type == x->damage_event_base + XDamageNotify) {
        if (x->stats) x->stats->damage_events++;
        XDamageNotifyEvent *dmg_ev = (XDamageNotifyEvent *)ev;
        WindowState *w = find_window_by_damage(x, dmg_ev->damage);
        if (w) {
            if (w->is_occluded && !x->full_redraw) {
                (void)register_damage_request(x, dmg_ev->damage, false, NULL);
                if (x->stats) x->stats->skipped_occluded_damage++;
                return;
            }
            /* NonEmpty reports only the first transition. Capture the complete
             * accumulated server region before clearing it, including later draws.
             * scene_damage is reused as scratch storage for this extraction. */
            if (x->scene_damage == None) {
                x->scene_damage = XFixesCreateRegion(x->display, NULL, 0);
                if (x->stats) x->stats->live_regions++;
            }
            unsigned long subtract_serial = 0;
            if (!register_damage_request(x, dmg_ev->damage, true, &subtract_serial)) {
                x->pending_reconcile = true;
                return;
            }

            XRectangle bounds = {0};
            int count = 0;
            if (x->stats) x->stats->x_round_trips++;
            unsigned long fetch_serial = NextRequest(x->display);
            XRectangle *parts = XFixesFetchRegionAndBounds(x->display, x->scene_damage, &count, &bounds);
            bool subtract_failed = complete_damage_requests_before(x, fetch_serial, &subtract_serial);
            if (subtract_failed) {
                if (parts) XFree(parts);
                return;
            }
            bool added = false;
            bool have_bounds = bounds.width > 0 && bounds.height > 0;
            if (have_bounds && (count > 0 || !parts)) {
                /* Bounds survive XFixes rectangle-allocation failure. */
                added = x11_damage_rect(x, w->x + w->border_width + bounds.x,
                    w->y + w->border_width + bounds.y, bounds.width, bounds.height);
            } else if (!parts && count > 0) {
                /* With no usable bounds, repaint the whole window conservatively. */
                added = x11_damage_window(x, w);
            }
            if (parts) XFree(parts);
            if (added) {
                x->pending_changes |= CHANGE_CONTENT;
                *dirty = true;
            }
        }
        return;
    }

    if (x->shape_event_base > 0 && ev->type == x->shape_event_base + ShapeNotify) {
        XShapeEvent *sev = (XShapeEvent *)ev;
        if (sev->window == x->overlay) {
            return;
        }
        if (sev->kind == ShapeBounding) {
            WindowState *w = x11_find_window(x, sev->window);
            if (!w) {
                w = find_window_by_opacity_client(x, sev->window);
            }
            if (w) {
                bool d1 = x11_damage_window(x, w);
                update_window_shape(x, w);
                bool d2 = x11_damage_window(x, w);
                x11_update_occlusion(x);
                x->pending_changes |= CHANGE_SHAPE;
                if (d1 || d2) *dirty = true;
                x->fullscreen_recheck = true;
            } else if (sev->window == x->bypass_client || sev->window == x->bypass_frame) {
                x->fullscreen_recheck = true;
            }
        }
        return;
    }

    switch (ev->type) {
    case MapNotify: {
        WindowState *w = x11_find_window(x, ev->xmap.window);
        if (w) {
            w->storage_dirty = true;
        }
        x->pending_changes |= CHANGE_HIERARCHY;
        x->pending_reconcile = true;
        x->fullscreen_recheck = true;
        break;
    }

    case UnmapNotify: {
        WindowState *w = x11_find_window(x, ev->xunmap.window);
        if (w) {
            w->storage_dirty = true;
        }
        x->pending_changes |= CHANGE_HIERARCHY;
        x->pending_reconcile = true;
        x->fullscreen_recheck = true;
        break;
    }

    case ReparentNotify: {
        WindowState *w = x11_find_window(x, ev->xreparent.window);
        if (w) {
            w->storage_dirty = true;
        }
        x->pending_changes |= CHANGE_HIERARCHY;
        x->pending_reconcile = true;
        x->fullscreen_recheck = true;
        break;
    }

    case DestroyNotify: {
        WindowState *w = x11_find_window(x, ev->xdestroywindow.window);
        if (w) {
            if (x11_damage_window(x, w)) {
                *dirty = true;
            }
            if (w->damage != None) {
                w->damage = None;
                if (x->stats && x->stats->live_damage > 0) x->stats->live_damage--;
            }
            w->xid = None;
            w->viewable = false;
        }
        x->pending_changes |= CHANGE_HIERARCHY;
        x->pending_reconcile = true;
        x->fullscreen_recheck = true;
        break;
    }

    case ConfigureNotify: {
        XConfigureEvent *ce = &ev->xconfigure;
        if (ce->window == x->root) {
            /* Reconcile confirmed dimensions before clipping output damage. */
            x->root_size_changed = true;
            x->pending_changes |= CHANGE_OUTPUT;
            *dirty = true;
            x->fullscreen_recheck = true;
        } else {
            WindowState *w = x11_find_window(x, ce->window);
            if (!w) {
                w = find_window_by_opacity_client(x, ce->window);
            }
            if (w) {
                int expected_w = ce->width + 2 * ce->border_width;
                int expected_h = ce->height + 2 * ce->border_width;
                if (w->width != expected_w || w->height != expected_h || w->border_width != ce->border_width) {
                    w->storage_dirty = true;
                }
            }
            x->pending_changes |= CHANGE_GEOMETRY;
            x->pending_reconcile = true;
            x->fullscreen_recheck = true;
        }
        break;
    }

    case CirculateNotify:
        x->pending_changes |= CHANGE_HIERARCHY;
        x->pending_reconcile = true;
        x->fullscreen_recheck = true;
        break;

    case PropertyNotify: {
        XPropertyEvent *pe = &ev->xproperty;
        if (pe->window == x->root && background_handles_atom(&x->background, pe->atom)) {
            x->background.refresh_required = true;
            x->pending_reconcile = true;
            x->fullscreen_recheck = true;
            break;
        }
        if (pe->atom == x->net_wm_bypass_compositor) {
            x->fullscreen_recheck = true;
            x->pending_reconcile = true;
            break;
        }
        if (pe->atom == x->net_wm_opacity) {
            WindowState *w = x11_find_window(x, pe->window);
            if (!w) {
                w = find_window_by_opacity_client(x, pe->window);
            }
            if (!w) {
                for (size_t i = 0; i < x->num_windows; ++i) {
                    WindowState *fw = &x->windows[i];
                    if (fw->opacity_client == None) {
                        fw->opacity_client = find_client_window(x, fw->xid, x->wm_state_atom);
                    }
                    if (fw->opacity_client == pe->window) {
                        w = fw;
                        break;
                    }
                }
            }
            if (w) {
                float old_op = w->opacity;
                w->opacity_dirty = true;
                update_window_opacity(x, w);
                w->opacity_dirty = false;
                if (w->opacity != old_op) {
                    if (x11_damage_window(x, w)) {
                        *dirty = true;
                    }
                    x11_update_occlusion(x);
                    x->pending_changes |= CHANGE_PROPERTY;
                }
            }
            x->fullscreen_recheck = true;
        } else if (pe->atom == x->wm_state_atom) {
            x->pending_changes |= CHANGE_HIERARCHY;
            x->pending_reconcile = true;
            x->fullscreen_recheck = true;
        }
        break;
    }

    case Expose:
        if (ev->xexpose.window == x->overlay) {
            /* Expose repairs overlay contents lost by the server. Keep any
             * already accumulated client damage; the explicit full intent is
             * enough to repaint the complete scene on the next required frame. */
            if (x->fullscreen_mode != FULLSCREEN_BYPASSED) {
                x->needs_full_repaint = true;
                *dirty = true;
            }
        }
        break;

    case SelectionClear:
        if (ev->xselectionclear.window == x->owner_win &&
            ev->xselectionclear.selection == x->net_wm_cm_s) {
            fprintf(stderr, "slopcom: Selection cleared by another manager; stopping\n");
            *stop_requested = true;
        }
        break;

    default:
        break;
    }
}

void x11_release_windows(X11State *x, EGLState *e) {
    if (!x->display) return;

    x11_finish_damage_batch(x);

    if (x->windows) {
        for (size_t i = 0; i < x->num_windows; ++i) {
            release_window_resources(x->display, e, &x->windows[i], x->stats);
        }
        free(x->windows);
        x->windows = NULL;
        x->num_windows = 0;
        x->cap_windows = 0;
    }

    x->history_count = 0;
    x->history_head = 0;

    if (x->scene_damage != None) {
        XFixesDestroyRegion(x->display, x->scene_damage);
        x->scene_damage = None;
        if (x->stats && x->stats->live_regions > 0) x->stats->live_regions--;
    }

    if (e) {
        background_release(&x->background, e);
    }
}

void x11_destroy(X11State *x, EGLState *e) {
    if (!x->display) {
        x11_uninstall_error_handler(x);
        return;
    }

    x11_finish_damage_batch(x);

    x11_release_windows(x, e);

    if (x->overlay_hidden && x->overlay != None) {
        x11_trap_errors(x->display);
        XFixesSetWindowShapeRegion(x->display, x->overlay, ShapeBounding, 0, 0, None);
        x11_untrap_errors(x->display, NULL);
        x->overlay_hidden = false;
    }

    if (x->overlay_acquired && x->overlay != None) {
        XCompositeReleaseOverlayWindow(x->display, x->overlay);
        x->overlay_acquired = false;
        x->overlay = None;
    }

    if (x->redirected) {
        x11_trap_errors(x->display);
        XCompositeUnredirectSubwindows(x->display, x->root, CompositeRedirectManual);
        x11_untrap_errors(x->display, NULL);
        x->redirected = false;
    }

    if (x->selection_acquired && x->owner_win != None) {
        if (XGetSelectionOwner(x->display, x->net_wm_cm_s) == x->owner_win) {
            XSetSelectionOwner(x->display, x->net_wm_cm_s, None, CurrentTime);
        }
        x->selection_acquired = false;
    }

    if (x->owner_win != None) {
        XDestroyWindow(x->display, x->owner_win);
        x->owner_win = None;
    }

    XSync(x->display, False);
    x11_uninstall_error_handler(x);
    XCloseDisplay(x->display);
    x->display = NULL;
}
