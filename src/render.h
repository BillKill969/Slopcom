#ifndef SLOPCOM_RENDER_H
#define SLOPCOM_RENDER_H

#include <stdbool.h>
#include <GLES2/gl2.h>

struct X11State;
struct BackgroundState;
struct EGLState;

typedef struct RenderState {
    GLuint program;
    GLint u_resolution_loc;
    GLint u_rect_loc;
    GLint u_uv_extent_loc;
    GLint u_texture_loc;
    GLint u_opacity_loc;
    GLint u_has_alpha_loc;
    GLuint vbo;
    bool full_redraw;
    bool has_npot_repeat;
    struct EGLState *egl;
} RenderState;

bool render_init(RenderState *r, int width, int height);
void draw_background(RenderState *r, struct BackgroundState *bg, int width, int height);
/* Return true only when a required repaint was drawn. */
bool render_scene(RenderState *r, struct X11State *x, int width, int height);
void render_destroy(RenderState *r);

#endif /* SLOPCOM_RENDER_H */
