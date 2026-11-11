#ifndef SLOPCOM_X11_ERRORS_H
#define SLOPCOM_X11_ERRORS_H

#include <X11/Xlib.h>

struct X11State;

typedef struct {
    Display *display;
    unsigned long start_serial;
    int caught;
    unsigned char error_code;
    unsigned char request_code;
    unsigned char minor_code;
    XID resourceid;
    unsigned long serial;
    int (*prev_handler)(Display *, XErrorEvent *);
} XErrorTrap;

/* Scoped synchronous X error trap functions */
void x11_trap_errors_scoped(Display *dpy, XErrorTrap *trap);
int x11_untrap_errors_scoped(XErrorTrap *trap);

/* Compatibility wrappers */
void x11_trap_errors(Display *dpy);
int x11_untrap_errors(Display *dpy, unsigned char *out_error_code);

/* Process-wide error handler lifecycle */
void x11_install_error_handler(struct X11State *x);
void x11_uninstall_error_handler(struct X11State *x);

#endif /* SLOPCOM_X11_ERRORS_H */
