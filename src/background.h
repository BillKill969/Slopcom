#ifndef SLOPCOM_BACKGROUND_H
#define SLOPCOM_BACKGROUND_H

#include <stdbool.h>
#include <X11/Xlib.h>
#include "egl.h"

typedef struct BackgroundState {
    Display *display;
    Window root;
    Atom primary_atom, fallback_atom, hint_atom;
    Pixmap pixmap;              /* Borrowed; never call XFreePixmap on it. */
    unsigned int width, height, depth, root_depth;
    ImportedPixmap imported;   /* Owned wrappers around the borrowed pixmap. */
    bool valid, refresh_required;
} BackgroundState;

void background_init(BackgroundState *bg, Display *display, Window root, unsigned int root_depth);
bool background_handles_atom(BackgroundState *bg, Atom atom);
void background_refresh(BackgroundState *bg, EGLState *egl);
void background_release(BackgroundState *bg, EGLState *egl);

#endif /* SLOPCOM_BACKGROUND_H */
