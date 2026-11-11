#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/damagewire.h>

#include "x11_errors.h"
#include "x11.h"

/* Scoped synchronous X error trap */
static XErrorTrap s_compat_trap;
static XErrorTrap *s_active_trap;

static int trap_error_handler_scoped(Display *dpy, XErrorEvent *ev) {
    XErrorTrap *trap = s_active_trap;
    if (!trap) return 0;
    /* Account for serial wrap */
    unsigned long diff = ev->serial - trap->start_serial;
    if (dpy == trap->display && diff < (1UL << 31)) {
        trap->caught = 1;
        trap->error_code = ev->error_code;
        trap->request_code = ev->request_code;
        trap->minor_code = ev->minor_code;
        trap->resourceid = ev->resourceid;
        trap->serial = ev->serial;
        return 0;
    }
    if (trap->prev_handler) {
        return trap->prev_handler(dpy, ev);
    }
    return 0;
}

void x11_trap_errors_scoped(Display *dpy, XErrorTrap *trap) {
    XSync(dpy, False);
    memset(trap, 0, sizeof(*trap));
    trap->display = dpy;
    trap->start_serial = NextRequest(dpy);
    trap->prev_handler = XSetErrorHandler(trap_error_handler_scoped);
    s_active_trap = trap;
}

int x11_untrap_errors_scoped(XErrorTrap *trap) {
    XSync(trap->display, False);
    XSetErrorHandler(trap->prev_handler);
    s_active_trap = NULL;
    return trap->caught;
}

void x11_trap_errors(Display *dpy) {
    x11_trap_errors_scoped(dpy, &s_compat_trap);
}

int x11_untrap_errors(Display *dpy, unsigned char *out_error_code) {
    (void)dpy;
    int caught = x11_untrap_errors_scoped(&s_compat_trap);
    if (out_error_code) {
        *out_error_code = s_compat_trap.error_code;
    }
    return caught;
}

static struct X11State *s_active_x11 = NULL;
static XErrorHandler s_prev_x_error_handler = NULL;

static int default_x_error_handler(Display *dpy, XErrorEvent *ev) {
    struct X11State *x = s_active_x11;
    if (x && dpy == x->display && x->damage_opcode != 0 &&
        ev->request_code == (unsigned char)x->damage_opcode &&
        ev->minor_code == X_DamageSubtract &&
        ev->error_code == (unsigned char)(x->damage_error_base + BadDamage)) {
        for (size_t i = 0; i < x->damage_request_count; ++i) {
            DamageRequest *request = &x->damage_requests[i];
            if (ev->serial == request->serial && ev->resourceid == request->damage) {
                /* Keep the callback side-effect free: reconciliation can move
                 * WindowState records while an XSync is dispatching errors. */
                request->failed = true;
                return 0;
            }
        }
    }

    char buf[256];
    XGetErrorText(dpy, ev->error_code, buf, sizeof(buf));
    fprintf(stderr, "slopcom: X error: code %d (%s), request %d.%d, serial %lu, resource 0x%lx\n",
            ev->error_code, buf, ev->request_code, ev->minor_code, ev->serial, ev->resourceid);
    return 0;
}

void x11_install_error_handler(struct X11State *x) {
    if (s_active_x11 == x) return;
    s_active_x11 = x;
    s_prev_x_error_handler = XSetErrorHandler(default_x_error_handler);
}

void x11_uninstall_error_handler(struct X11State *x) {
    if (s_active_x11 == x) {
        s_active_x11 = NULL;
        XSetErrorHandler(s_prev_x_error_handler);
        s_prev_x_error_handler = NULL;
    }
}
