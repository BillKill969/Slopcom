#include "background.h"
#include "x11_errors.h"
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool read_pixmap(Display *dpy, Window root, Atom atom, Pixmap *out) {
    Atom type = None;
    int format = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char *data = NULL;
    *out = None;
    if (atom == None) return false;
    int rc = XGetWindowProperty(dpy, root, atom, 0, 1, False, XA_PIXMAP,
        &type, &format, &count, &remaining, &data);
    if (rc == Success && type == XA_PIXMAP && format == 32 &&
        count == 1 && remaining == 0 && data) {
        *out = (Pixmap)(*(unsigned long *)data & 0xffffffffUL);
    }
    if (data) XFree(data);
    return *out != None;
}

void background_init(BackgroundState *bg, Display *display, Window root, unsigned int root_depth) {
    memset(bg, 0, sizeof(*bg));
    bg->display = display;
    bg->root = root;
    bg->root_depth = root_depth;
    if (display) {
        bg->primary_atom = XInternAtom(display, "_XROOTPMAP_ID", False);
        bg->fallback_atom = XInternAtom(display, "ESETROOT_PMAP_ID", False);
        bg->hint_atom = XInternAtom(display, "_XSETROOT_ID", False);
    }
    bg->pixmap = None;
    bg->refresh_required = true;
    bg->valid = false;
}

bool background_handles_atom(BackgroundState *bg, Atom atom) {
    if (atom == None) return false;
    return (atom == bg->primary_atom || atom == bg->fallback_atom || atom == bg->hint_atom);
}

static bool validate_pixmap_geometry(BackgroundState *bg, Pixmap p, unsigned int *out_w, unsigned int *out_h, unsigned int *out_d) {
    Window root_ret = None;
    int x_ret = 0, y_ret = 0;
    unsigned int w_ret = 0, h_ret = 0, border_ret = 0, depth_ret = 0;

    x11_trap_errors(bg->display);
    Status st = XGetGeometry(bg->display, p, &root_ret, &x_ret, &y_ret, &w_ret, &h_ret, &border_ret, &depth_ret);
    unsigned char err = 0;
    int untrap_rc = x11_untrap_errors(bg->display, &err);

    if (st == 0 || untrap_rc != 0 || root_ret != bg->root || w_ret == 0 || h_ret == 0) {
        return false;
    }
    if (depth_ret != bg->root_depth && depth_ret != 24 && depth_ret != 32) {
        return false;
    }

    *out_w = w_ret;
    *out_h = h_ret;
    *out_d = depth_ret;
    return true;
}

void background_refresh(BackgroundState *bg, EGLState *egl) {
    bg->refresh_required = false;

    Atom atoms[] = { bg->primary_atom, bg->fallback_atom };
    Pixmap attempted = None;
    for (size_t i = 0; i < sizeof(atoms) / sizeof(atoms[0]); ++i) {
        Pixmap candidate = None;
        unsigned int width = 0, height = 0, depth = 0;
        if (!read_pixmap(bg->display, bg->root, atoms[i], &candidate) ||
            candidate == attempted) continue;
        attempted = candidate;
        if (!validate_pixmap_geometry(bg, candidate, &width, &height, &depth)) continue;

        /* A refreshed XID must retire its old EGL wrapper before re-import. */
        if (candidate == bg->pixmap) background_release(bg, egl);
        ImportedPixmap imported = {0};
        if (!egl_import_pixmap(egl, candidate, &imported)) continue;

        background_release(bg, egl);
        bg->imported = imported;
        bg->pixmap = candidate;
        bg->width = width;
        bg->height = height;
        bg->depth = depth;
        bg->valid = true;
        return;
    }
    background_release(bg, egl);
}

void background_release(BackgroundState *bg, EGLState *egl) {
    if (bg->imported.image != EGL_NO_IMAGE_KHR || bg->imported.texture != 0) {
        egl_release_import(egl, &bg->imported);
    }
    bg->pixmap = None;
    bg->valid = false;
    bg->width = 0;
    bg->height = 0;
    bg->depth = 0;
}
