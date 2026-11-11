#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xrender.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

static void sleep_us(long us) {
    struct timespec ts;
    ts.tv_sec = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000;
    nanosleep(&ts, NULL);
}

static Visual *find_argb_visual(Display *dpy, int screen, int *depth_out) {
    XVisualInfo vinfo_template;
    vinfo_template.screen = screen;
    vinfo_template.depth = 32;
    vinfo_template.class = TrueColor;

    int num_visuals = 0;
    XVisualInfo *vlist = XGetVisualInfo(dpy, VisualScreenMask | VisualDepthMask | VisualClassMask,
                                        &vinfo_template, &num_visuals);
    if (!vlist) return NULL;

    Visual *found = NULL;
    for (int i = 0; i < num_visuals; ++i) {
        XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, vlist[i].visual);
        if (fmt && fmt->type == PictTypeDirect && fmt->direct.alphaMask != 0) {
            found = vlist[i].visual;
            *depth_out = vlist[i].depth;
            break;
        }
    }
    XFree(vlist);
    return found;
}

static void apply_shape_hole(Display *dpy, Window win, int width, int height) {
    int hw = width / 4;
    int hh = height / 4;
    if (hw < 1) hw = 1;
    if (hh < 1) hh = 1;
    if (height <= 2 * hh || width <= 2 * hw) return;

    /* 4 rectangles forming a hollow frame around a center hole (YXBanded order) */
    XRectangle rects[4];
    rects[0].x = 0; rects[0].y = 0; rects[0].width = width; rects[0].height = hh;
    rects[1].x = 0; rects[1].y = hh; rects[1].width = hw; rects[1].height = height - 2 * hh;
    rects[2].x = width - hw; rects[2].y = hh; rects[2].width = hw; rects[2].height = height - 2 * hh;
    rects[3].x = 0; rects[3].y = height - hh; rects[3].width = width; rects[3].height = hh;

    XShapeCombineRectangles(dpy, win, ShapeBounding, 0, 0, rects, 4, ShapeSet, Unsorted);
}

static void remove_shape(Display *dpy, Window win, int width, int height) {
    XRectangle r = { 0, 0, (unsigned short)width, (unsigned short)height };
    XShapeCombineRectangles(dpy, win, ShapeBounding, 0, 0, &r, 1, ShapeSet, Unsorted);
}

static void set_opacity(Display *dpy, Window win, float opacity) {
    Atom net_wm_opacity = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
    if (opacity >= 1.0f) {
        XDeleteProperty(dpy, win, net_wm_opacity);
    } else {
        uint32_t val = (uint32_t)(opacity * 4294967295.0f);
        XChangeProperty(dpy, win, net_wm_opacity, XA_CARDINAL, 32,
                        PropModeReplace, (unsigned char *)&val, 1);
    }
}

static void draw_asymmetric_rgb(Display *dpy, Window win, GC gc, int width, int height, uint32_t frame) {
    int half_w = width / 2;
    int half_h = height / 2;

    /* Animated quadrants based on frame number */
    unsigned long c0 = (frame & 1) ? 0xFF0000 : 0xCC0000;
    unsigned long c1 = (frame & 2) ? 0x00FF00 : 0x00CC00;
    unsigned long c2 = (frame & 4) ? 0x0000FF : 0x0000CC;
    unsigned long c3 = 0xFFFF00;

    XSetForeground(dpy, gc, c0);
    XFillRectangle(dpy, win, gc, 0, 0, half_w, half_h);

    XSetForeground(dpy, gc, c1);
    XFillRectangle(dpy, win, gc, half_w, 0, width - half_w, half_h);

    XSetForeground(dpy, gc, c2);
    XFillRectangle(dpy, win, gc, 0, half_h, half_w, height - half_h);

    XSetForeground(dpy, gc, c3);
    XFillRectangle(dpy, win, gc, half_w, half_h, width - half_w, height - half_h);

    XFlush(dpy);
}

static void draw_argb_xrender(Display *dpy, Window win, int width, int height, uint32_t frame) {
    XRenderPictFormat *fmt = XRenderFindStandardFormat(dpy, PictStandardARGB32);
    XRenderPictureAttributes pa;
    Picture pic = XRenderCreatePicture(dpy, win, fmt, 0, &pa);

    unsigned short alpha_val = (unsigned short)(0x6000 + ((frame % 16) * 0x0800));
    XRenderColor c_outer = { 0x4000, 0x0000, 0x8000, alpha_val };
    XRenderColor c_inner = { 0x0000, 0x6000, 0x6000, 0x8000 };

    XRenderFillRectangle(dpy, PictOpSrc, pic, &c_outer, 0, 0, width, height);
    XRenderFillRectangle(dpy, PictOpSrc, pic, &c_inner, width / 8, height / 8, width * 3 / 4, height * 3 / 4);

    XRenderFreePicture(dpy, pic);
    XFlush(dpy);
}

static int run_churn(Display *dpy, int duration_sec) {
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    #define CHURN_MAX_WINDOWS 8
    struct ChurnWin {
        Window win;
        bool mapped;
        bool shaped;
        int x, y, w, h;
    } wins[CHURN_MAX_WINDOWS];
    memset(wins, 0, sizeof(wins));

    printf("fixture: Starting churn mode for %d seconds\n", duration_sec);
    time_t start = time(NULL);
    uint32_t cycle = 0;

    while (duration_sec <= 0 || (time(NULL) - start) < duration_sec) {
        int idx = cycle % CHURN_MAX_WINDOWS;
        struct ChurnWin *cw = &wins[idx];

        if (cw->win == None) {
            /* Create new window */
            cw->w = 150 + (rand() % 200);
            cw->h = 100 + (rand() % 150);
            cw->x = 50 + (rand() % 400);
            cw->y = 50 + (rand() % 300);

            XSetWindowAttributes attrs;
            memset(&attrs, 0, sizeof(attrs));
            attrs.override_redirect = (rand() % 2 == 0) ? True : False;
            attrs.background_pixel = (rand() % 2 == 0) ? WhitePixel(dpy, screen) : BlackPixel(dpy, screen);
            attrs.border_pixel = BlackPixel(dpy, screen);

            cw->win = XCreateWindow(dpy, root, cw->x, cw->y, cw->w, cw->h, 2,
                                    DefaultDepth(dpy, screen), InputOutput,
                                    DefaultVisual(dpy, screen),
                                    CWOverrideRedirect | CWBackPixel | CWBorderPixel,
                                    &attrs);
            char name[64];
            snprintf(name, sizeof(name), "Churn Window #%d (cycle %u)", idx, cycle);
            XStoreName(dpy, cw->win, name);
            XMapWindow(dpy, cw->win);
            cw->mapped = true;
            cw->shaped = false;
        } else {
            int action = rand() % 6;
            switch (action) {
            case 0: /* Map/unmap */
                if (cw->mapped) {
                    XUnmapWindow(dpy, cw->win);
                    cw->mapped = false;
                } else {
                    XMapWindow(dpy, cw->win);
                    cw->mapped = true;
                }
                break;
            case 1: /* Move */
                cw->x = 50 + (rand() % 400);
                cw->y = 50 + (rand() % 300);
                XMoveWindow(dpy, cw->win, cw->x, cw->y);
                break;
            case 2: /* Resize */
                cw->w = 150 + (rand() % 200);
                cw->h = 100 + (rand() % 150);
                XResizeWindow(dpy, cw->win, cw->w, cw->h);
                if (cw->shaped) apply_shape_hole(dpy, cw->win, cw->w, cw->h);
                break;
            case 3: /* Shape toggle */
                if (cw->shaped) {
                    remove_shape(dpy, cw->win, cw->w, cw->h);
                    cw->shaped = false;
                } else {
                    apply_shape_hole(dpy, cw->win, cw->w, cw->h);
                    cw->shaped = true;
                }
                break;
            case 4: /* Opacity change */
                {
                    float op = (float)(rand() % 100) / 100.0f;
                    set_opacity(dpy, cw->win, op);
                }
                break;
            case 5: /* Destroy & recreate */
                XDestroyWindow(dpy, cw->win);
                cw->win = None;
                cw->mapped = false;
                cw->shaped = false;
                break;
            }
        }

        XFlush(dpy);
        sleep_us(50000); /* 50ms per step */
        cycle++;
    }

    /* Cleanup remaining windows */
    for (int i = 0; i < CHURN_MAX_WINDOWS; ++i) {
        if (wins[i].win != None) {
            XDestroyWindow(dpy, wins[i].win);
            wins[i].win = None;
        }
    }
    XFlush(dpy);
    printf("fixture: Churn completed %u steps\n", cycle);
    return 0;
}

int main(int argc, char **argv) {
    const char *mode = "both";
    const char *workload = NULL;
    int duration_sec = 0;
    int max_frames = 0;
    int border_width = 2;
    float opacity = 1.0f;
    bool managed = false;
    bool shape_hole = false;
    bool shape_cycle = false;
    bool opacity_cycle = false;
    int fps = 30;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            mode = argv[++i];
        } else if (!strcmp(argv[i], "--workload") && i + 1 < argc) {
            workload = argv[++i];
        } else if (!strcmp(argv[i], "--duration") && i + 1 < argc) {
            duration_sec = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            max_frames = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--border") && i + 1 < argc) {
            border_width = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--opacity") && i + 1 < argc) {
            opacity = (float)atof(argv[++i]);
        } else if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
            fps = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--managed")) {
            managed = true;
        } else if (!strcmp(argv[i], "--override")) {
            managed = false;
        } else if (!strcmp(argv[i], "--shape-hole")) {
            shape_hole = true;
        } else if (!strcmp(argv[i], "--shape-cycle")) {
            shape_cycle = true;
        } else if (!strcmp(argv[i], "--opacity-cycle")) {
            opacity_cycle = true;
        } else if (argv[i][0] != '-') {
            mode = argv[i];
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                duration_sec = atoi(argv[++i]);
            }
        }
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "fixture: Cannot open X display\n");
        return 1;
    }

    if (!strcmp(mode, "churn")) {
        int rc = run_churn(dpy, duration_sec);
        XCloseDisplay(dpy);
        return rc;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    Window rgb_win = None;
    Window argb_win = None;
    Window gl_win = None;
    EGLDisplay egl_dpy = EGL_NO_DISPLAY;
    EGLSurface egl_surf = EGL_NO_SURFACE;
    EGLContext egl_ctx = EGL_NO_CONTEXT;

    bool want_rgb = (!strcmp(mode, "rgb") || !strcmp(mode, "both") || !strcmp(mode, "all"));
    bool want_argb = (!strcmp(mode, "argb") || !strcmp(mode, "both") || !strcmp(mode, "all"));
    bool want_gl = (!strcmp(mode, "gl") || !strcmp(mode, "all"));

    if (want_rgb) {
        XSetWindowAttributes attrs;
        memset(&attrs, 0, sizeof(attrs));
        attrs.override_redirect = managed ? False : True;
        attrs.background_pixel = WhitePixel(dpy, screen);
        attrs.border_pixel = BlackPixel(dpy, screen);
        attrs.event_mask = ExposureMask | StructureNotifyMask;

        rgb_win = XCreateWindow(dpy, root, 100, 100, 320, 240, border_width,
                                DefaultDepth(dpy, screen), InputOutput,
                                DefaultVisual(dpy, screen),
                                CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask,
                                &attrs);
        XStoreName(dpy, rgb_win, "Fixture RGB Window (Asymmetric)");
        if (shape_hole) apply_shape_hole(dpy, rgb_win, 320, 240);
        if (opacity < 1.0f) set_opacity(dpy, rgb_win, opacity);
        XMapWindow(dpy, rgb_win);
    }

    if (want_argb) {
        int depth = 0;
        Visual *argb_visual = find_argb_visual(dpy, screen, &depth);
        if (argb_visual) {
            Colormap cmap = XCreateColormap(dpy, root, argb_visual, AllocNone);
            XSetWindowAttributes attrs;
            memset(&attrs, 0, sizeof(attrs));
            attrs.colormap = cmap;
            attrs.border_pixel = 0;
            attrs.background_pixel = 0;
            attrs.override_redirect = managed ? False : True;
            attrs.event_mask = ExposureMask | StructureNotifyMask;

            argb_win = XCreateWindow(dpy, root, 250, 200, 320, 240, border_width,
                                     depth, InputOutput, argb_visual,
                                     CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect | CWEventMask,
                                     &attrs);
            XStoreName(dpy, argb_win, "Fixture ARGB Window (Translucent)");
            if (shape_hole) apply_shape_hole(dpy, argb_win, 320, 240);
            if (opacity < 1.0f) set_opacity(dpy, argb_win, opacity);
            XMapWindow(dpy, argb_win);
        }
    }

    if (want_gl) {
        egl_dpy = eglGetDisplay((EGLNativeDisplayType)dpy);
        if (egl_dpy != EGL_NO_DISPLAY && eglInitialize(egl_dpy, NULL, NULL)) {
            const EGLint cfg_attribs[] = {
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                EGL_NONE
            };
            EGLConfig cfg;
            EGLint num_cfg = 0;
            if (eglChooseConfig(egl_dpy, cfg_attribs, &cfg, 1, &num_cfg) && num_cfg > 0) {
                EGLint vid = 0;
                eglGetConfigAttrib(egl_dpy, cfg, EGL_NATIVE_VISUAL_ID, &vid);
                XVisualInfo vtmpl = { .visualid = (VisualID)vid, .screen = screen };
                int nvi = 0;
                XVisualInfo *vi = XGetVisualInfo(dpy, VisualIDMask | VisualScreenMask, &vtmpl, &nvi);
                if (vi) {
                    Colormap cmap = XCreateColormap(dpy, root, vi->visual, AllocNone);
                    XSetWindowAttributes attrs;
                    memset(&attrs, 0, sizeof(attrs));
                    attrs.colormap = cmap;
                    attrs.override_redirect = managed ? False : True;
                    attrs.event_mask = ExposureMask | StructureNotifyMask;

                    gl_win = XCreateWindow(dpy, root, 400, 150, 320, 240, border_width,
                                           vi->depth, InputOutput, vi->visual,
                                           CWColormap | CWOverrideRedirect | CWEventMask, &attrs);
                    XStoreName(dpy, gl_win, "Fixture GL Producer Window");
                    XMapWindow(dpy, gl_win);
                    XFree(vi);

                    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
                    egl_ctx = eglCreateContext(egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
                    egl_surf = eglCreateWindowSurface(egl_dpy, cfg, (EGLNativeWindowType)gl_win, NULL);
                    eglMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx);
                    eglSwapInterval(egl_dpy, 1);
                }
            }
        }
    }

    XSync(dpy, False);

    GC rgb_gc = NULL;
    if (rgb_win != None) {
        rgb_gc = XCreateGC(dpy, rgb_win, 0, NULL);
        draw_asymmetric_rgb(dpy, rgb_win, rgb_gc, 320, 240, 0);
    }
    if (argb_win != None) {
        draw_argb_xrender(dpy, argb_win, 320, 240, 0);
    }
    XFlush(dpy);

    if (workload && !strcmp(workload, "idle")) {
        if (duration_sec > 0) sleep(duration_sec);
        else pause();
        goto finish;
    }

    if (workload && !strcmp(workload, "typing")) fps = 8;
    else if (workload && !strcmp(workload, "drag-resize")) fps = 20;
    else if (workload && !strcmp(workload, "scrolling")) fps = 30;
    else if (workload && !strcmp(workload, "video")) fps = 60;

    uint32_t frame = 0;
    time_t start_time = time(NULL);
    useconds_t frame_delay_us = (fps > 0) ? (1000000 / fps) : 33333;

    while (true) {
        while (XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);
        }

        if (workload && !strcmp(workload, "typing")) {
            if (rgb_win != None) {
                int cx = 20 + (int)((frame % 30) * 8);
                XSetForeground(dpy, rgb_gc, (frame & 1) ? 0x000000 : 0xFFFFFF);
                XFillRectangle(dpy, rgb_win, rgb_gc, cx, 50, 6, 16);
                XFlush(dpy);
            }
        } else if (workload && !strcmp(workload, "drag-resize")) {
            if (rgb_win != None) {
                int dw = 320 + ((frame % 20 < 10) ? 40 : 0);
                int dh = 240 + ((frame % 20 < 10) ? 30 : 0);
                int dx = 100 + ((frame % 20 < 10) ? 20 : 0);
                int dy = 100 + ((frame % 20 < 10) ? 15 : 0);
                XMoveResizeWindow(dpy, rgb_win, dx, dy, dw, dh);
                draw_asymmetric_rgb(dpy, rgb_win, rgb_gc, dw, dh, frame);
            }
        } else if (workload && !strcmp(workload, "scrolling")) {
            if (rgb_win != None) {
                int y_off = (int)((frame * 8) % 240);
                XSetForeground(dpy, rgb_gc, 0x333333);
                XFillRectangle(dpy, rgb_win, rgb_gc, 0, y_off, 320, 10);
                XFlush(dpy);
            }
        } else {
            if (rgb_win != None) {
                draw_asymmetric_rgb(dpy, rgb_win, rgb_gc, 320, 240, frame);
            }
            if (argb_win != None) {
                draw_argb_xrender(dpy, argb_win, 320, 240, frame);
            }
            if (gl_win != None && egl_surf != EGL_NO_SURFACE) {
                float g_col = (float)((frame % 60)) / 60.0f;
                glClearColor(0.2f, g_col, 0.8f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                eglSwapBuffers(egl_dpy, egl_surf);
            }
        }

        if (shape_cycle && (frame % 30 == 0)) {
            bool shaped_now = ((frame / 30) % 2 == 1);
            if (rgb_win != None) {
                if (shaped_now) apply_shape_hole(dpy, rgb_win, 320, 240);
                else remove_shape(dpy, rgb_win, 320, 240);
            }
            if (argb_win != None) {
                if (shaped_now) apply_shape_hole(dpy, argb_win, 320, 240);
                else remove_shape(dpy, argb_win, 320, 240);
            }
        }

        if (opacity_cycle && (frame % 15 == 0)) {
            static const float ops[] = { 1.0f, 0.8f, 0.5f, 0.3f };
            float cur_op = ops[(frame / 15) % 4];
            if (rgb_win != None) set_opacity(dpy, rgb_win, cur_op);
            if (argb_win != None) set_opacity(dpy, argb_win, cur_op);
        }

        frame++;
        if (max_frames > 0 && frame >= (uint32_t)max_frames) {
            break;
        }
        if (duration_sec > 0 && (time(NULL) - start_time) >= duration_sec) {
            break;
        }
        sleep_us(frame_delay_us);
    }

finish:
    if (rgb_gc) XFreeGC(dpy, rgb_gc);
    if (gl_win != None) {
        eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_ctx != EGL_NO_CONTEXT) eglDestroyContext(egl_dpy, egl_ctx);
        if (egl_surf != EGL_NO_SURFACE) eglDestroySurface(egl_dpy, egl_surf);
        if (egl_dpy != EGL_NO_DISPLAY) eglTerminate(egl_dpy);
        XDestroyWindow(dpy, gl_win);
    }
    if (rgb_win != None) XDestroyWindow(dpy, rgb_win);
    if (argb_win != None) XDestroyWindow(dpy, argb_win);

    XSync(dpy, False);
    XCloseDisplay(dpy);
    return 0;
}
