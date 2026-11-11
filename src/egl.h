#ifndef SLOPCOM_EGL_H
#define SLOPCOM_EGL_H

#include <stdbool.h>
#include <stdint.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <X11/Xlib.h>
#include "damage.h"

struct Stats;

typedef struct {
    EGLImageKHR image;
    GLuint texture;
} ImportedPixmap;

typedef struct EGLState {
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface surface;

    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind_image;

    bool is_platform_ext;
    bool is_initialized;
    bool has_current_context;
    bool full_redraw;
    bool no_vsync;
    bool has_buffer_age;
    bool has_partial_update;
    bool has_swap_buffers_with_damage;
    PFNEGLSETDAMAGEREGIONKHRPROC set_damage_region;
    PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC swap_buffers_with_damage;
    EGLint presentation_damage[4 * MAX_DAMAGE_RECTS];
    int presentation_damage_count;
    struct Stats *stats;
} EGLState;

bool egl_init(EGLState *e, Display *xdisplay, Window overlay, VisualID overlay_visual_id, int width, int height);
bool egl_import_pixmap(EGLState *e, Pixmap pixmap, ImportedPixmap *out);
void egl_release_import(EGLState *e, ImportedPixmap *imp);
bool egl_present(EGLState *e);
void egl_destroy(EGLState *e, Display *xdisplay);

#endif /* SLOPCOM_EGL_H */
