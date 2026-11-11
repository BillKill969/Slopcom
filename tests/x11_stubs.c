#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>
#include "x11_stubs.h"
#include "x11.h"
#include "stats.h"

/* Exercise the real reconciliation/event code without a desktop or GPU. */
int imports, releases;
bool import_ok = true, exists = true, vanish_on_import;
Pixmap backing = 100;
int client_width = 320, client_height = 240, border_width = 2;
int window_x, window_y;
static Visual visual = { .visualid = 24 };
static XErrorHandler error_handler;
int tree_window_count = 1;
Window tree_windows[8] = { 42, 43, 0, 0, 0, 0, 0, 0 };
bool fail_on_window_43 = false;
bool empty_shape, translucent_property;
XRectangle damage_bounds = { 0, 0, 320, 240 };
XserverRegion captured_damage;
bool stub_fetch_region_alloc_fail;
bool stub_fetch_region_empty;

int free_pixmap_calls = 0;
StubProp stub_primary_prop;
StubProp stub_fallback_prop;
StubProp stub_hint_prop;
bool stub_geometry_fail = false;
bool stub_query_tree_fail = false;
bool stub_geometry_override = false;
Window stub_geometry_root = 1;
unsigned int stub_geometry_width = 0;
unsigned int stub_geometry_height = 0;
unsigned int stub_geometry_depth = 24;
Pixmap stub_fail_import_pixmap;
Window stub_frame_client;
bool stub_frame_has_wm_state = false;
long stub_window_masks[3];
bool stub_damage_subtract_error = false;
Damage stub_error_damage_id = 0;
unsigned long stub_last_damage_subtract_serial = 0;
bool stub_damage_create_error = false;
unsigned char stub_damage_create_error_code = BadDrawable;
int stub_fixes_set_region_calls = 0;
int stub_sync_calls = 0;
int stub_pending_events = 0;
int stub_serial_attr_calls = 0;
int stub_xcb_op_count = 0;
int stub_xcb_send_count = 0;
int stub_xcb_recv_count = 0;
int stub_xcb_outstanding = 0;
int stub_xcb_invalid_replies = 0;
int stub_xcb_flush_count = 0;
int stub_xflush_calls = 0;

StubXCBLogEntry stub_xcb_ops[MAX_STUB_XCB_OPS];
bool stub_xcb_connection_error = false;
bool stub_xcb_flush_failure = false;
int stub_xcb_disconnect_after_replies = 0;
static bool connection_failure_observed;
Window stub_xcb_fail_attr_window = None;
Window stub_xcb_fail_geom_window = None;
unsigned char stub_xcb_attr_error_code = 0;
unsigned char stub_xcb_geom_error_code = 0;
Window stub_xcb_missing_reply_window = None;
Window stub_xcb_input_only_window = None;
Window stub_xcb_unmapped_window = None;
Window stub_xcb_override_redirect_window = None;
bool stub_client_override_redirect = false;
bool stub_client_shaped = false;
VisualID stub_client_visual_id = 24;
bool stub_client_has_custom_geom = false;
int stub_client_w = 0, stub_client_h = 0, stub_client_border_width = 0;
bool stub_bypass_hint_set = false;
unsigned long stub_bypass_hint_value = 0;
bool stub_bypass_hint_malformed = false;
bool stub_opacity_set = false;
unsigned long stub_opacity_value = 0xffffffffUL;
bool stub_opacity_malformed = false;
int stub_composite_redirect_calls = 0;
int stub_composite_unredirect_calls = 0;
bool stub_composite_redirect_fail = false;
bool stub_composite_unredirect_fail = false;
bool stub_root_redirected = true;
int stub_shape_set_bounding_calls = 0;
int stub_shape_set_input_calls = 0;
unsigned long stub_shape_masks[3];
bool stub_shape_region_fail = false;
XserverRegion stub_last_bounding_shape_region = 0;
int stub_translate_coords_calls = 0;
bool stub_translate_coords_fail = false;
int stub_translate_dest_x = 0;
int stub_translate_dest_y = 0;
VisualID stub_xcb_window_visual_id[16];
int stub_xcb_window_x[16];
int stub_xcb_window_y[16];
int stub_xcb_window_w[16];
int stub_xcb_window_h[16];
int stub_xcb_window_bw[16];
bool stub_xcb_has_custom_geom[16];

typedef struct {
    bool used;
    int kind; /* 1: attr, 2: geom */
    Window window;
    Display *display;
    unsigned long serial;
} StubXCBRequest;
#define MAX_STUB_XCB_REQUESTS 256
static StubXCBRequest s_stub_requests[MAX_STUB_XCB_REQUESTS];

static Visual stub_visual_24 = {
    .visualid = 24,
    .class = TrueColor,
    .bits_per_rgb = 8,
    .red_mask = 0xff0000,
    .green_mask = 0x00ff00,
    .blue_mask = 0x0000ff
};

static Visual stub_visual_32 = {
    .visualid = 32,
    .class = TrueColor,
    .bits_per_rgb = 8,
    .red_mask = 0x00ff0000,
    .green_mask = 0x0000ff00,
    .blue_mask = 0x000000ff
};

static Depth stub_depths[2] = {
    { .depth = 24, .nvisuals = 1, .visuals = &stub_visual_24 },
    { .depth = 32, .nvisuals = 1, .visuals = &stub_visual_32 }
};

static Screen stub_screen = {
    .ndepths = 2,
    .depths = stub_depths,
    .root_depth = 24,
    .width = 1000,
    .height = 1000
};

static void stub_init_display_screens(Display *dpy) {
    _XPrivDisplay private_display = (_XPrivDisplay)dpy;
    if (private_display && !private_display->screens) {
        private_display->nscreens = 1;
        private_display->default_screen = 0;
        private_display->screens = &stub_screen;
    }
}

typedef struct {
    XErrorEvent event;
    bool pending;
} StubPendingError;
static StubPendingError pending_damage_errors[256];

static unsigned long stub_issue_request(Display *dpy) {
    /* Fail distinctly if connection-loss handling attempts server cleanup. */
    if (connection_failure_observed) _exit(99);
    _XPrivDisplay private_display = (_XPrivDisplay)dpy;
    return ++private_display->request;
}

static bool stub_serial_before(unsigned long a, unsigned long b) {
    return (long)(a - b) < 0;
}

static void stub_deliver_errors_before(Display *dpy, unsigned long fence) {
    for (size_t i = 0; i < sizeof(pending_damage_errors) / sizeof(pending_damage_errors[0]); ++i) {
        StubPendingError *pending = &pending_damage_errors[i];
        if (pending->pending && pending->event.display == dpy &&
            stub_serial_before(pending->event.serial, fence)) {
            pending->pending = false;
            if (error_handler) error_handler(dpy, &pending->event);
        }
    }
}

void reset_x11_stubs(void) {
    imports = releases = free_pixmap_calls = 0;
    import_ok = exists = true;
    vanish_on_import = fail_on_window_43 = empty_shape = translucent_property = false;
    stub_fetch_region_alloc_fail = stub_fetch_region_empty = false;
    stub_damage_subtract_error = false;
    stub_error_damage_id = 0;
    stub_last_damage_subtract_serial = 0;
    stub_damage_create_error = false;
    stub_damage_create_error_code = BadDrawable;
    stub_fixes_set_region_calls = 0;
    stub_sync_calls = 0;
    stub_pending_events = 0;
    memset(pending_damage_errors, 0, sizeof(pending_damage_errors));
    backing = 100;
    client_width = 320; client_height = 240; border_width = 2;
    window_x = window_y = 0;
    tree_window_count = 1;
    tree_windows[0] = 42; tree_windows[1] = 43;
    memset(&stub_primary_prop, 0, sizeof(stub_primary_prop));
    memset(&stub_fallback_prop, 0, sizeof(stub_fallback_prop));
    memset(&stub_hint_prop, 0, sizeof(stub_hint_prop));
    stub_geometry_fail = false;
    stub_query_tree_fail = false;
    stub_geometry_override = false;
    stub_geometry_root = 1;
    stub_geometry_width = 0;
    stub_geometry_height = 0;
    stub_geometry_depth = 24;
    stub_fail_import_pixmap = None;
    stub_frame_client = None;
    stub_frame_has_wm_state = false;
    memset(stub_window_masks, 0, sizeof(stub_window_masks));
    stub_xcb_op_count = 0;
    stub_xcb_send_count = 0;
    stub_xcb_recv_count = 0;
    stub_xcb_outstanding = 0;
    stub_xcb_invalid_replies = 0;
    stub_xcb_flush_count = 0;
    stub_xflush_calls = 0;
    stub_serial_attr_calls = 0;
    stub_xcb_connection_error = false;
    stub_xcb_flush_failure = false;
    stub_xcb_disconnect_after_replies = 0;
    connection_failure_observed = false;
    stub_xcb_fail_attr_window = None;
    stub_xcb_fail_geom_window = None;
    stub_xcb_attr_error_code = 0;
    stub_xcb_geom_error_code = 0;
    stub_xcb_missing_reply_window = None;
    stub_xcb_input_only_window = None;
    stub_xcb_unmapped_window = None;
    stub_xcb_override_redirect_window = None;
    stub_client_override_redirect = false;
    stub_client_shaped = false;
    stub_client_visual_id = 24;
    stub_client_has_custom_geom = false;
    stub_client_w = stub_client_h = stub_client_border_width = 0;
    stub_bypass_hint_set = false;
    stub_bypass_hint_value = 0;
    stub_bypass_hint_malformed = false;
    stub_opacity_set = false;
    stub_opacity_value = 0xffffffffUL;
    stub_opacity_malformed = false;
    stub_composite_redirect_calls = 0;
    stub_composite_unredirect_calls = 0;
    stub_composite_redirect_fail = false;
    stub_composite_unredirect_fail = false;
    stub_root_redirected = true;
    stub_shape_set_bounding_calls = 0;
    stub_shape_set_input_calls = 0;
    memset(stub_shape_masks, 0, sizeof(stub_shape_masks));
    stub_shape_region_fail = false;
    stub_last_bounding_shape_region = 0;
    stub_translate_coords_calls = 0;
    stub_translate_coords_fail = false;
    stub_translate_dest_x = 0;
    stub_translate_dest_y = 0;
    memset(stub_xcb_ops, 0, sizeof(stub_xcb_ops));
    memset(stub_xcb_window_visual_id, 0, sizeof(stub_xcb_window_visual_id));
    memset(stub_xcb_window_x, 0, sizeof(stub_xcb_window_x));
    memset(stub_xcb_window_y, 0, sizeof(stub_xcb_window_y));
    memset(stub_xcb_window_w, 0, sizeof(stub_xcb_window_w));
    memset(stub_xcb_window_h, 0, sizeof(stub_xcb_window_h));
    memset(stub_xcb_window_bw, 0, sizeof(stub_xcb_window_bw));
    memset(stub_xcb_has_custom_geom, 0, sizeof(stub_xcb_has_custom_geom));
    memset(s_stub_requests, 0, sizeof(s_stub_requests));
}

void stub_emit_x_error(Display *display, unsigned char code) {
    XErrorEvent ev = { .display = display, .serial = stub_issue_request(display),
        .error_code = code, .resourceid = 42 };
    error_handler(display, &ev);
}

void stub_emit_x_error_event(const XErrorEvent *event) {
    if (event && error_handler) error_handler(event->display, (XErrorEvent *)event);
}

int XSync(Display *dpy, Bool discard) {
    (void)discard;
    stub_sync_calls++;
    unsigned long sync_serial = stub_issue_request(dpy);
    stub_deliver_errors_before(dpy, sync_serial);
    return 0;
}
int XPending(Display *dpy) {
    (void)dpy;
    return stub_pending_events;
}
XErrorHandler XSetErrorHandler(XErrorHandler handler) {
    XErrorHandler old = error_handler;
    error_handler = handler;
    return old;
}
int XGetErrorText(Display *dpy, int code, char *buffer, int length) {
    (void)dpy;
    const char *message = code == BadDamage ? "BadDamage" : "stub X error";
    if (length <= 0) return 0;
    snprintf(buffer, (size_t)length, "%s", message);
    return 0;
}
Status XQueryTree(Display *dpy, Window w, Window *root, Window *parent,
                  Window **children, unsigned int *count) {
    (void)dpy;
    *root = *parent = 1;
    if (w == 1 && stub_query_tree_fail) {
        *children = NULL;
        *count = 0;
        return 0;
    }
    if (w == 42 && stub_frame_client != None && stub_frame_client != 42) {
        *count = 1;
        *children = malloc(sizeof(Window));
        **children = stub_frame_client;
        return 1;
    }
    *count = (w == 1) ? (unsigned int)tree_window_count : 0;
    if (*count) {
        *children = malloc(sizeof(Window) * (*count));
        for (unsigned int i = 0; i < *count; ++i) {
            (*children)[i] = tree_windows[i];
        }
    } else {
        *children = NULL;
    }
    return 1;
}
Status XGetWindowAttributes(Display *dpy, Window w, XWindowAttributes *a) {
    (void)dpy; (void)w;
    stub_serial_attr_calls++;
    memset(a, 0, sizeof(*a));
    a->x = window_x; a->y = window_y;
    a->width = client_width; a->height = client_height; a->border_width = border_width;
    a->depth = 24; a->visual = &visual;
    a->class = InputOutput; a->map_state = IsViewable;
    if (w >= 42 && w <= 44) a->your_event_mask = stub_window_masks[w - 42];
    if (stub_frame_client != None && w == stub_frame_client) {
        if (stub_client_has_custom_geom) {
            a->width = stub_client_w;
            a->height = stub_client_h;
            a->border_width = stub_client_border_width;
        }
        if (stub_client_visual_id == 32) {
            a->visual = &stub_visual_32;
            a->depth = 32;
        }
        if (stub_client_override_redirect) {
            a->override_redirect = True;
        }
    }
    return exists;
}
int XSelectInput(Display *dpy, Window w, long mask) {
    (void)dpy;
    if (w >= 42 && w <= 44) stub_window_masks[w - 42] = mask;
    return 1;
}
Atom XInternAtom(Display *dpy, const char *atom_name, Bool only_if_exists) {
    (void)dpy; (void)only_if_exists;
    if (!atom_name) return None;
    if (!strcmp(atom_name, "_XROOTPMAP_ID")) return 1001;
    if (!strcmp(atom_name, "ESETROOT_PMAP_ID")) return 1002;
    if (!strcmp(atom_name, "_XSETROOT_ID")) return 1003;
    if (!strcmp(atom_name, "_NET_WM_WINDOW_OPACITY")) return 77;
    if (!strcmp(atom_name, "WM_STATE")) return 88;
    if (!strcmp(atom_name, "_NET_WM_BYPASS_COMPOSITOR")) return 99;
    return 999;
}
int XFree(void *data) {
    free(data);
    return 1;
}
XRenderPictFormat *XRenderFindVisualFormat(Display *dpy, const Visual *v) {
    (void)dpy;
    static XRenderPictFormat fmt24 = { .type = PictTypeDirect, .direct = { .alphaMask = 0 } };
    static XRenderPictFormat fmt32 = { .type = PictTypeDirect, .direct = { .alphaMask = 0xff } };
    if (v && v->visualid == 32) return &fmt32;
    return &fmt24;
}
int XGetWindowProperty(Display *dpy, Window w, Atom property, long offset,
    long length, Bool del, Atom requested, Atom *type, int *format,
    unsigned long *count, unsigned long *remaining, unsigned char **data) {
    (void)dpy; (void)w; (void)property; (void)offset; (void)length;
    (void)del; (void)requested;
    *type = None; *format = 0; *count = *remaining = 0; *data = NULL;
    if (property == 99) {
        if (!stub_bypass_hint_set) return Success;
        if (stub_bypass_hint_malformed) {
            *type = XA_STRING; *format = 8; *count = 1;
            unsigned char *val = malloc(1);
            if (val) { *val = 1; *data = val; }
            return Success;
        }
        *type = XA_CARDINAL; *format = 32; *count = 1;
        unsigned long *val = malloc(sizeof(*val));
        if (val) { *val = stub_bypass_hint_value; *data = (unsigned char *)val; }
        return Success;
    }
    if (property == 77 && stub_opacity_set) {
        if (stub_opacity_malformed) {
            *type = XA_STRING; *format = 8; *count = 1;
            unsigned char *val = malloc(1);
            if (val) { *val = 1; *data = val; }
            return Success;
        }
        *type = XA_CARDINAL; *format = 32; *count = 1;
        unsigned long *val = malloc(sizeof(*val));
        if (val) { *val = stub_opacity_value; *data = (unsigned char *)val; }
        return Success;
    }
    if ((stub_frame_has_wm_state && w == 42 && property == 88) ||
        (stub_frame_client != None && w == stub_frame_client && (property == 88 || property == 77))) {
        unsigned long *value = calloc(2, sizeof(*value));
        value[0] = (w == stub_frame_client && property == 77) ? (w == 43 ? 0x80000000UL : 0xbfffffffUL) : 1;
        *type = (w == stub_frame_client && property == 77) ? XA_CARDINAL : 88; *format = 32;
        *count = (w == stub_frame_client && property == 77) ? 1 : 2; *data = (unsigned char *)value;
        return Success;
    }
    if (translucent_property && w == 43 && property == 77) {
        unsigned long *value = malloc(sizeof(*value));
        *value = 0x80000000UL;
        *type = XA_CARDINAL; *format = 32; *count = 1;
        *data = (unsigned char *)value;
        return Success;
    }
    if (w == 1) {
        StubProp *sp = NULL;
        if (property == 1001) sp = &stub_primary_prop;
        else if (property == 1002) sp = &stub_fallback_prop;
        else if (property == 1003) sp = &stub_hint_prop;
        if (sp && sp->is_set) {
            *type = sp->type;
            *format = sp->format;
            *count = sp->count;
            *remaining = sp->remaining;
            if (sp->count > 0) {
                unsigned long *val = malloc(sizeof(unsigned long) * sp->count);
                if (val) {
                    for (unsigned long i = 0; i < sp->count; ++i) val[i] = sp->value;
                    *data = (unsigned char *)val;
                }
            }
            return Success;
        }
    }
    return Success;
}
Damage XDamageCreate(Display *dpy, Drawable drawable, int level) {
    (void)level;
    unsigned long serial = stub_issue_request(dpy);
    if (stub_damage_create_error && error_handler) {
        XErrorEvent ev = { .display = dpy, .serial = serial, .error_code = stub_damage_create_error_code,
            .request_code = 140, .minor_code = 1, .resourceid = drawable };
        error_handler(dpy, &ev);
    }
    return (Damage)(200 + (drawable >= 42 ? (drawable - 42) : 0));
}
void XDamageDestroy(Display *dpy, Damage damage) { (void)damage; (void)stub_issue_request(dpy); }
void XDamageSubtract(Display *dpy, Damage damage, XserverRegion repair, XserverRegion parts) {
    (void)repair;
    captured_damage = parts;
    unsigned long serial = stub_issue_request(dpy);
    stub_last_damage_subtract_serial = serial;
    if (stub_damage_subtract_error && (!stub_error_damage_id || damage == stub_error_damage_id)) {
        XErrorEvent ev = {
            .display = dpy,
            .serial = serial,
            .error_code = 143 /* damage_error_base (143) + BadDamage (0) */,
            .request_code = 140 /* damage_opcode */,
            .minor_code = 3 /* X_DamageSubtract */,
            .resourceid = damage
        };
        for (size_t i = 0; i < sizeof(pending_damage_errors) / sizeof(pending_damage_errors[0]); ++i) {
            if (!pending_damage_errors[i].pending) {
                pending_damage_errors[i].event = ev;
                pending_damage_errors[i].pending = true;
                break;
            }
        }
    }
}
Pixmap XCompositeNameWindowPixmap(Display *dpy, Window w) {
    (void)stub_issue_request(dpy);
    if (w >= 42 && w <= 57 && stub_xcb_has_custom_geom[w - 42]) {
        return (Pixmap)(100 + (w - 42));
    }
    return backing;
}
Status XGetGeometry(Display *dpy, Drawable drawable, Window *root, int *x, int *y,
    unsigned int *width, unsigned int *height, unsigned int *border, unsigned int *depth) {
    (void)drawable; (void)stub_issue_request(dpy);
    if (stub_geometry_fail) return 0;
    if (stub_geometry_override) {
        *root = stub_geometry_root;
        *x = *y = 0;
        *width = stub_geometry_width;
        *height = stub_geometry_height;
        *border = 0;
        *depth = stub_geometry_depth;
        return 1;
    }
    if (drawable >= 100 && drawable <= 115 && stub_xcb_has_custom_geom[drawable - 100]) {
        int idx = (int)(drawable - 100);
        *root = 1;
        *x = *y = 0;
        *width = (unsigned int)(stub_xcb_window_w[idx] + 2 * stub_xcb_window_bw[idx]);
        *height = (unsigned int)(stub_xcb_window_h[idx] + 2 * stub_xcb_window_bw[idx]);
        *border = 0;
        *depth = (stub_xcb_window_visual_id[idx] == 32) ? 32 : 24;
        return 1;
    }
    *root = 1; *x = *y = 0;
    *width = (unsigned int)(client_width + 2 * border_width);
    *height = (unsigned int)(client_height + 2 * border_width);
    *border = 0; *depth = 24;
    return 1;
}
int XFreePixmap(Display *dpy, Pixmap pixmap) {
    (void)pixmap; (void)stub_issue_request(dpy);
    free_pixmap_calls++;
    return 1;
}

void XShapeSelectInput(Display *dpy, Window w, unsigned long mask) {
    (void)dpy;
    if (w >= 42 && w <= 44) stub_shape_masks[w - 42] = mask;
}
Status XShapeQueryExtents(Display *dpy, Window w, Bool *b_shaped, int *bx, int *by, unsigned int *bw, unsigned int *bh,
                         Bool *c_shaped, int *cx, int *cy, unsigned int *cw, unsigned int *ch) {
    (void)dpy; (void)w; (void)bx; (void)by; (void)bw; (void)bh; (void)cx; (void)cy; (void)cw; (void)ch;
    *b_shaped = (stub_frame_client != None && w == stub_frame_client && stub_client_shaped) ? True : empty_shape;
    *c_shaped = False;
    return 1;
}
XRectangle *XShapeGetRectangles(Display *dpy, Window window, int kind, int *count, int *ordering) {
    (void)dpy; (void)window; (void)kind; *count = *ordering = 0; return NULL;
}

XserverRegion XFixesCreateRegion(Display *dpy, XRectangle *rects, int n) {
    (void)dpy; (void)rects; (void)n;
    return 1;
}
void XFixesDestroyRegion(Display *dpy, XserverRegion r) { (void)dpy; (void)r; }
void XFixesTranslateRegion(Display *dpy, XserverRegion r, int dx, int dy) { (void)dpy; (void)r; (void)dx; (void)dy; }
void XFixesUnionRegion(Display *dpy, XserverRegion d, XserverRegion s1, XserverRegion s2) { (void)dpy; (void)d; (void)s1; (void)s2; }
void XFixesIntersectRegion(Display *dpy, XserverRegion d, XserverRegion s1, XserverRegion s2) { (void)dpy; (void)d; (void)s1; (void)s2; }
void XFixesSubtractRegion(Display *dpy, XserverRegion d, XserverRegion s1, XserverRegion s2) { (void)dpy; (void)d; (void)s1; (void)s2; }
void XFixesSetRegion(Display *dpy, XserverRegion r, XRectangle *rects, int n) {
    (void)r; (void)rects; (void)n; (void)stub_issue_request(dpy);
    stub_fixes_set_region_calls++;
}
void XFixesCopyRegion(Display *dpy, XserverRegion dst, XserverRegion src) { (void)dpy; (void)dst; (void)src; }
XRectangle *XFixesFetchRegion(Display *dpy, XserverRegion r, int *n) { (void)dpy; (void)r; *n = 0; return NULL; }
XRectangle *XFixesFetchRegionAndBounds(Display *dpy, XserverRegion r, int *n, XRectangle *b) {
    (void)r;
    unsigned long fetch_serial = stub_issue_request(dpy);
    stub_deliver_errors_before(dpy, fetch_serial);
    if (stub_fetch_region_empty) {
        *n = 0; *b = (XRectangle){0};
        return NULL;
    }
    if (stub_fetch_region_alloc_fail) {
        *n = 0; *b = damage_bounds;
        return NULL;
    }
    *n = 1; *b = damage_bounds;
    XRectangle *rects = malloc(sizeof(*rects));
    if (rects) *rects = damage_bounds;
    return rects;
}
void XFixesSetWindowShapeRegion(Display *dpy, Window win, int kind, int x_off, int y_off, XserverRegion region) {
    (void)win; (void)x_off; (void)y_off;
    unsigned long serial = stub_issue_request(dpy);
    if (kind == ShapeBounding) {
        stub_shape_set_bounding_calls++;
        stub_last_bounding_shape_region = region;
    } else if (kind == ShapeInput) {
        stub_shape_set_input_calls++;
    }
    if (stub_shape_region_fail && error_handler) {
        XErrorEvent ev = { .display = dpy, .serial = serial, .error_code = BadRegion,
                           .request_code = 142, .minor_code = 2, .resourceid = region };
        error_handler(dpy, &ev);
    }
}

Bool XTranslateCoordinates(Display *dpy, Window src_w, Window dest_w,
                           int src_x, int src_y, int *dest_x_return,
                           int *dest_y_return, Window *child_return) {
    (void)src_w; (void)dest_w; (void)src_x; (void)src_y;
    stub_translate_coords_calls++;
    (void)stub_issue_request(dpy);
    if (stub_translate_coords_fail) return False;
    if (dest_x_return) *dest_x_return = stub_translate_dest_x;
    if (dest_y_return) *dest_y_return = stub_translate_dest_y;
    if (child_return) *child_return = None;
    return True;
}

void XCompositeRedirectSubwindows(Display *dpy, Window window, int update) {
    (void)window; (void)update;
    stub_composite_redirect_calls++;
    unsigned long serial = stub_issue_request(dpy);
    if (stub_composite_redirect_fail && error_handler) {
        XErrorEvent ev = { .display = dpy, .serial = serial, .error_code = BadAccess,
                           .request_code = 150, .minor_code = 1, .resourceid = window };
        error_handler(dpy, &ev);
        return;
    }
    stub_root_redirected = true;
}

void XCompositeUnredirectSubwindows(Display *dpy, Window window, int update) {
    (void)window; (void)update;
    stub_composite_unredirect_calls++;
    unsigned long serial = stub_issue_request(dpy);
    if (stub_composite_unredirect_fail && error_handler) {
        XErrorEvent ev = { .display = dpy, .serial = serial, .error_code = BadAccess,
                           .request_code = 150, .minor_code = 2, .resourceid = window };
        error_handler(dpy, &ev);
        return;
    }
    stub_root_redirected = false;
}

bool egl_import_pixmap(EGLState *e, Pixmap pixmap, ImportedPixmap *out) {
    if (fail_on_window_43 && imports == 1) {
        return false;
    }
    imports++;
    if (vanish_on_import) exists = false;
    if (!import_ok || pixmap == stub_fail_import_pixmap) return false;
    out->image = (EGLImageKHR)(uintptr_t)pixmap; out->texture = (GLuint)pixmap;
    if (e && e->stats) { e->stats->live_images++; e->stats->live_textures++; }
    return true;
}
void egl_release_import(EGLState *e, ImportedPixmap *out) {
    releases++;
    if (out->image != EGL_NO_IMAGE_KHR) {
        if (e && e->stats && e->stats->live_images > 0) e->stats->live_images--;
        out->image = EGL_NO_IMAGE_KHR;
    }
    if (out->texture != 0) {
        if (e && e->stats && e->stats->live_textures > 0) e->stats->live_textures--;
        out->texture = 0;
    }
}

xcb_connection_t *XGetXCBConnection(Display *dpy) {
    stub_init_display_screens(dpy);
    return (xcb_connection_t *)dpy;
}

int XFlush(Display *dpy) {
    (void)dpy;
    stub_xflush_calls++;
    return 1;
}

static void log_xcb_operation(StubXCBOpKind op, Window window, unsigned int sequence) {
    if (stub_xcb_op_count < MAX_STUB_XCB_OPS)
        stub_xcb_ops[stub_xcb_op_count++] = (StubXCBLogEntry){ op, window, sequence };
}

int xcb_flush(xcb_connection_t *c) {
    (void)c;
    stub_xcb_flush_count++;
    log_xcb_operation(STUB_XCB_OP_FLUSH, None, 0);
    if (stub_xcb_flush_failure) {
        stub_xcb_connection_error = true;
        connection_failure_observed = true;
        return 0;
    }
    return 1;
}

int xcb_connection_has_error(xcb_connection_t *c) {
    (void)c;
    if (stub_xcb_connection_error) connection_failure_observed = true;
    return stub_xcb_connection_error ? 1 : 0;
}

static unsigned int queue_xcb_request(xcb_connection_t *c, Window window, int kind) {
    for (size_t i = 0; i < MAX_STUB_XCB_REQUESTS; ++i) {
        StubXCBRequest *request = &s_stub_requests[i];
        if (request->used) continue;
        Display *display = (Display *)c;
        unsigned long serial = stub_issue_request(display);
        *request = (StubXCBRequest){
            .used = true, .kind = kind, .window = window,
            .display = display, .serial = serial
        };
        stub_xcb_send_count++;
        stub_xcb_outstanding++;
        log_xcb_operation(kind == 1 ? STUB_XCB_OP_ATTR_SEND : STUB_XCB_OP_GEOM_SEND,
                          window, (unsigned int)serial);
        return (unsigned int)serial;
    }
    fputs("FAIL: offline XCB request capacity exhausted\n", stderr);
    abort();
}

static StubXCBRequest *consume_xcb_reply(xcb_connection_t *c, unsigned int sequence, int kind) {
    if (stub_xcb_flush_failure && connection_failure_observed) _exit(98);
    for (size_t i = 0; i < MAX_STUB_XCB_REQUESTS; ++i) {
        StubXCBRequest *request = &s_stub_requests[i];
        if (!request->used || request->display != (Display *)c ||
            (unsigned int)request->serial != sequence || request->kind != kind) continue;
        request->used = false;
        stub_xcb_outstanding--;
        stub_xcb_recv_count++;
        log_xcb_operation(kind == 1 ? STUB_XCB_OP_ATTR_RECV : STUB_XCB_OP_GEOM_RECV,
                          request->window, sequence);
        stub_deliver_errors_before(request->display, request->serial);
        if (stub_xcb_disconnect_after_replies > 0 &&
            stub_xcb_recv_count == stub_xcb_disconnect_after_replies) {
            stub_xcb_connection_error = true;
            connection_failure_observed = true;
        }
        return request;
    }
    stub_xcb_invalid_replies++;
    return NULL;
}

static xcb_generic_error_t *query_error(const StubXCBRequest *request, unsigned char code) {
    xcb_generic_error_t *error = calloc(1, sizeof(*error));
    if (error) {
        error->error_code = code;
        error->resource_id = (uint32_t)request->window;
        error->major_code = request->kind == 1 ? XCB_GET_WINDOW_ATTRIBUTES : XCB_GET_GEOMETRY;
        error->sequence = (uint16_t)request->serial;
        error->full_sequence = (unsigned int)request->serial;
    }
    return error;
}

xcb_get_window_attributes_cookie_t xcb_get_window_attributes(xcb_connection_t *c, xcb_window_t window) {
    return (xcb_get_window_attributes_cookie_t){ queue_xcb_request(c, window, 1) };
}

xcb_get_geometry_cookie_t xcb_get_geometry(xcb_connection_t *c, xcb_drawable_t drawable) {
    return (xcb_get_geometry_cookie_t){ queue_xcb_request(c, drawable, 2) };
}

xcb_get_window_attributes_reply_t *xcb_get_window_attributes_reply(xcb_connection_t *c,
                                                                   xcb_get_window_attributes_cookie_t cookie,
                                                                   xcb_generic_error_t **e) {
    if (e) *e = NULL;
    StubXCBRequest *request = consume_xcb_reply(c, cookie.sequence, 1);
    if (!request) return NULL;
    Window w = request->window;
    if (stub_xcb_connection_error || stub_xcb_missing_reply_window == w) return NULL;
    if (stub_xcb_fail_attr_window == w) {
        if (e) *e = query_error(request, stub_xcb_attr_error_code ? stub_xcb_attr_error_code : BadWindow);
        return NULL;
    }
    xcb_get_window_attributes_reply_t *reply = calloc(1, sizeof(*reply));
    if (!reply) return NULL;
    reply->response_type = 1;
    reply->sequence = (uint16_t)cookie.sequence;
    reply->_class = (w == stub_xcb_input_only_window) ? XCB_WINDOW_CLASS_INPUT_ONLY : XCB_WINDOW_CLASS_INPUT_OUTPUT;
    reply->map_state = (w == stub_xcb_unmapped_window) ? XCB_MAP_STATE_UNMAPPED : XCB_MAP_STATE_VIEWABLE;
    if (w >= 42 && w <= 44) reply->your_event_mask = stub_window_masks[w - 42];
    VisualID vid = stub_xcb_window_visual_id[w % 16];
    reply->visual = (xcb_visualid_t)(vid ? vid : 24);
    reply->override_redirect = (w == stub_xcb_override_redirect_window) ? 1 : 0;
    return reply;
}

xcb_get_geometry_reply_t *xcb_get_geometry_reply(xcb_connection_t *c,
                                                 xcb_get_geometry_cookie_t cookie,
                                                 xcb_generic_error_t **e) {
    if (e) *e = NULL;
    StubXCBRequest *request = consume_xcb_reply(c, cookie.sequence, 2);
    if (!request) return NULL;
    Window w = request->window;
    if (stub_xcb_connection_error || stub_xcb_missing_reply_window == w) return NULL;
    if (stub_xcb_fail_geom_window == w) {
        if (e) *e = query_error(request, stub_xcb_geom_error_code ? stub_xcb_geom_error_code : BadDrawable);
        return NULL;
    }
    xcb_get_geometry_reply_t *reply = calloc(1, sizeof(*reply));
    if (!reply) return NULL;
    reply->response_type = 1;
    reply->sequence = (uint16_t)cookie.sequence;
    reply->root = 1;
    VisualID vid = stub_xcb_window_visual_id[w % 16];
    reply->depth = (vid == 32) ? 32 : 24;
    if (w >= 42 && w <= 57 && stub_xcb_has_custom_geom[w - 42]) {
        int idx = (int)(w - 42);
        reply->x = (int16_t)stub_xcb_window_x[idx];
        reply->y = (int16_t)stub_xcb_window_y[idx];
        reply->width = (uint16_t)stub_xcb_window_w[idx];
        reply->height = (uint16_t)stub_xcb_window_h[idx];
        reply->border_width = (uint16_t)stub_xcb_window_bw[idx];
    } else {
        reply->x = (int16_t)window_x;
        reply->y = (int16_t)window_y;
        reply->width = (uint16_t)client_width;
        reply->height = (uint16_t)client_height;
        reply->border_width = (uint16_t)border_width;
    }
    return reply;
}
