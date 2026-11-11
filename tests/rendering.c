#include "x11_stubs.h"
#include "render.h"

/* Observe actual rendering decisions without connecting to X or a GPU. */
static bool scissor_enabled;
static bool blend_enabled = true;
static int full_clears, draws, swaps, viewport_width, viewport_height;
static GLuint bound_texture;
static int drawn_textures[8];
static int scissor_x, scissor_y, scissor_width, scissor_height;
static EGLint buffer_age = 2;

typedef struct {
    char op;
    bool scissor_enabled;
    bool blend_enabled;
    int x, y, width, height;
    float rect_x, rect_y, rect_w, rect_h;
    float uv_u, uv_v;
} RenderOp;
#define MAX_RENDER_OPS 32
static RenderOp render_ops[MAX_RENDER_OPS];
static int render_op_count = 0;
static float last_rect_x, last_rect_y, last_rect_w, last_rect_h;
static float last_uv_u, last_uv_v;
static int stub_buffer_data_calls = 0;
static int stub_delete_buffers_calls = 0;
static int stub_delete_program_calls = 0;
static int invalid_uniform_calls, invalid_vertex_state;
static GLuint bound_buffer;
static float uploaded_quad[24];
static bool quad_uploaded, require_initialized_geometry;
static bool attributes_enabled[2], attributes_bound[2];

enum {
    LOC_RESOLUTION = 11, LOC_RECT = 13, LOC_UV_EXTENT = 17,
    LOC_TEXTURE = 19, LOC_OPACITY = 23, LOC_HAS_ALPHA = 29
};

static RenderState test_render_state(void) {
    return (RenderState){
        .program = 99, .vbo = 42,
        .u_resolution_loc = LOC_RESOLUTION, .u_rect_loc = LOC_RECT,
        .u_uv_extent_loc = LOC_UV_EXTENT, .u_texture_loc = LOC_TEXTURE,
        .u_opacity_loc = LOC_OPACITY, .u_has_alpha_loc = LOC_HAS_ALPHA
    };
}

/* Exercise real render_init() while keeping all GL work offline. */
const GLubyte *glGetString(GLenum name) {
    return (const GLubyte *)(name == GL_VERSION ? "OpenGL ES 2.0" : "");
}
GLuint glCreateShader(GLenum type) { return type == GL_VERTEX_SHADER ? 1 : 2; }
void glShaderSource(GLuint s, GLsizei n, const GLchar *const *p, const GLint *length) {
    (void)s; (void)n; (void)p; (void)length;
}
void glCompileShader(GLuint s) { (void)s; }
void glGetShaderiv(GLuint s, GLenum name, GLint *value) {
    (void)s; *value = name == GL_COMPILE_STATUS ? GL_TRUE : 0;
}
void glDeleteShader(GLuint s) { (void)s; }
GLuint glCreateProgram(void) { return 99; }
void glAttachShader(GLuint p, GLuint s) { (void)p; (void)s; }
void glBindAttribLocation(GLuint p, GLuint i, const GLchar *name) {
    (void)p; (void)i; (void)name;
}
void glLinkProgram(GLuint p) { (void)p; }
void glGetProgramiv(GLuint p, GLenum name, GLint *value) {
    (void)p; *value = name == GL_LINK_STATUS ? GL_TRUE : 0;
}
GLint glGetUniformLocation(GLuint p, const GLchar *name) {
    (void)p;
    const struct { const char *name; GLint location; } uniforms[] = {
        {"u_resolution", LOC_RESOLUTION}, {"u_rect", LOC_RECT},
        {"u_uv_extent", LOC_UV_EXTENT}, {"u_texture", LOC_TEXTURE},
        {"u_opacity", LOC_OPACITY}, {"u_has_alpha", LOC_HAS_ALPHA}
    };
    for (size_t i = 0; i < sizeof(uniforms) / sizeof(uniforms[0]); ++i)
        if (!strcmp(name, uniforms[i].name)) return uniforms[i].location;
    return -1;
}
void glGenBuffers(GLsizei n, GLuint *buffers) {
    for (GLsizei i = 0; i < n; ++i) buffers[i] = 42 + i;
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    (void)x; (void)y; viewport_width = w; viewport_height = h;
}
void glEnable(GLenum c) {
    if (c == GL_SCISSOR_TEST) scissor_enabled = true;
    if (c == GL_BLEND) blend_enabled = true;
}
void glDisable(GLenum c) {
    if (c == GL_SCISSOR_TEST) scissor_enabled = false;
    if (c == GL_BLEND) blend_enabled = false;
}
void glClear(GLbitfield mask) {
    (void)mask;
    if (render_op_count < MAX_RENDER_OPS) {
        render_ops[render_op_count++] = (RenderOp){
            .op = 'C',
            .scissor_enabled = scissor_enabled,
            .blend_enabled = blend_enabled,
            .x = scissor_x, .y = scissor_y,
            .width = scissor_width, .height = scissor_height
        };
    }
    if (!scissor_enabled || (scissor_x == 0 && scissor_y == 0 &&
        scissor_width == viewport_width && scissor_height == viewport_height)) full_clears++;
}
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { (void)r; (void)g; (void)b; (void)a; }
void glUseProgram(GLuint p) { (void)p; }
void glUniform4f(GLint l, GLfloat x, GLfloat y, GLfloat w, GLfloat h) {
    if (l != LOC_RECT) { invalid_uniform_calls++; return; }
    last_rect_x = x;
    last_rect_y = y;
    last_rect_w = w;
    last_rect_h = h;
}
void glUniform2f(GLint l, GLfloat a, GLfloat b) {
    if (l == LOC_RESOLUTION) return;
    if (l != LOC_UV_EXTENT) { invalid_uniform_calls++; return; }
    last_uv_u = a;
    last_uv_v = b;
}
void glUniform1i(GLint l, GLint a) {
    (void)a;
    if (l != LOC_TEXTURE && l != LOC_HAS_ALPHA) invalid_uniform_calls++;
}
void glUniform1f(GLint l, GLfloat a) {
    (void)a;
    if (l != LOC_OPACITY) invalid_uniform_calls++;
}
void glActiveTexture(GLenum t) { (void)t; }
void glBlendFunc(GLenum s, GLenum d) { (void)s; (void)d; }
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    scissor_x = x; scissor_y = y; scissor_width = w; scissor_height = h;
}
void glBindTexture(GLenum t, GLuint texture) { (void)t; bound_texture = texture; }
void glTexParameteri(GLenum t, GLenum n, GLint v) { (void)t; (void)n; (void)v; }
void glBindBuffer(GLenum t, GLuint b) { (void)t; bound_buffer = b; }
void glBufferData(GLenum t, GLsizeiptr n, const void *p, GLenum u) {
    stub_buffer_data_calls++;
    if (t != GL_ARRAY_BUFFER || bound_buffer != 42 ||
        n != sizeof(uploaded_quad) || !p || u != GL_STATIC_DRAW) {
        invalid_vertex_state++;
        return;
    }
    memcpy(uploaded_quad, p, sizeof(uploaded_quad));
    quad_uploaded = true;
}
void glDeleteBuffers(GLsizei n, const GLuint *buffers) {
    (void)n; (void)buffers;
    stub_delete_buffers_calls++;
    quad_uploaded = false;
}
void glDeleteProgram(GLuint p) {
    (void)p;
    stub_delete_program_calls++;
}
void glEnableVertexAttribArray(GLuint a) { if (a < 2) attributes_enabled[a] = true; }
void glDisableVertexAttribArray(GLuint a) { if (a < 2) attributes_enabled[a] = false; }
void glVertexAttribPointer(GLuint i, GLint n, GLenum t, GLboolean norm, GLsizei stride, const void *p) {
    if (i < 2) attributes_bound[i] = bound_buffer == 42 && n == 2 &&
        t == GL_FLOAT && norm == GL_FALSE && stride == 4 * sizeof(float) &&
        (uintptr_t)p == i * 2 * sizeof(float);
}
void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (require_initialized_geometry && (!quad_uploaded ||
        !attributes_enabled[0] || !attributes_enabled[1] ||
        !attributes_bound[0] || !attributes_bound[1] ||
        mode != GL_TRIANGLES || first != 0 || count != 6)) invalid_vertex_state++;
    draws++;
    if (render_op_count < MAX_RENDER_OPS) {
        render_ops[render_op_count++] = (RenderOp){
            .op = 'D',
            .scissor_enabled = scissor_enabled,
            .blend_enabled = blend_enabled,
            .x = scissor_x, .y = scissor_y,
            .width = scissor_width, .height = scissor_height,
            .rect_x = last_rect_x, .rect_y = last_rect_y,
            .rect_w = last_rect_w, .rect_h = last_rect_h,
            .uv_u = last_uv_u, .uv_v = last_uv_v
        };
    }
    if (bound_texture < 8) drawn_textures[bound_texture]++;
}
EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint a, EGLint *v) {
    (void)d; (void)s; (void)a; *v = buffer_age; return EGL_TRUE;
}
bool egl_present(EGLState *e) { (void)e; swaps++; return true; }

static bool covers(const DamageList *list, int px, int py) {
    for (int i = 0; i < list->count; i++) {
        const DamageRect *r = &list->rects[i];
        if (px >= r->x && px < r->x + r->width && py >= r->y && py < r->y + r->height) return true;
    }
    return false;
}

#define CHECK(c, message) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", message); return 1; } } while (0)

int main(int argc, char **argv) {
    CHECK(argc == 2, "select a rendering regression");
    _XPrivDisplay display = calloc(1, sizeof(*display));
    CHECK(display != NULL, "allocate fake display");
    WindowState *windows = calloc(2, sizeof(*windows));
    CHECK(windows != NULL, "allocate windows");
    for (int i = 0; i < 2; i++) {
        windows[i] = (WindowState){ .xid = 42 + i, .viewable = true,
            .width = 324, .height = 244, .border_width = 2, .opacity = 1,
            .pixmap = 100 + i, .damage = 200 + i,
            .imported = { .image = (EGLImageKHR)(uintptr_t)(i + 1), .texture = i + 1 } };
    }
    Stats test_stats;
    stats_init(&test_stats);
    X11State x = { .display = (Display *)display, .root = 1, .root_width = 1000, .root_height = 1000,
        .windows = windows, .num_windows = 2, .damage_event_base = 80, .shape_event_base = 90,
        .net_wm_opacity = 77, .stats = &test_stats };
    EGLState e = {0};
    bool dirty = false, stop = false;
    XEvent ev = {0};
    RenderState default_renderer = test_render_state();
    if (!strcmp(argv[1], "fullscreen-entry-recovery")) {
        free(windows);
        windows = NULL;
        x = (X11State){
            .display = (Display *)display, .root = 1,
            .root_width = 1000, .root_height = 1000,
            .redirected = true, .overlay = 9999,
            .unredir_fullscreen = true, .fullscreen_recheck = true,
            .net_wm_opacity = 77, .wm_state_atom = 88,
            .net_wm_bypass_compositor = 99, .stats = &test_stats
        };
        EGLState e = {0};

        for (int failure = 0; failure < 2; ++failure) {
            if (failure > 0) {
                x11_release_windows(&x, &e);
                x.unredir_fullscreen = true;
                x.fullscreen_recheck = true;
                x.fullscreen_mode = FULLSCREEN_COMPOSITED;
                x.redirected = true;
                x.overlay_hidden = false;
            }
            reset_x11_stubs();
            tree_window_count = 1;
            tree_windows[0] = 42;
            window_x = window_y = 0;
            client_width = client_height = 1000;
            border_width = 0;
            stub_frame_has_wm_state = true;
            CHECK(x11_reconcile(&x, &e), "capture fullscreen client before failed entry");

            if (failure == 0) {
                stub_composite_unredirect_fail = true;
            } else {
                stub_shape_region_fail = true;
            }
            bool dirty = true;
            CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
                  "recover storage after failed entry operation");
            CHECK(x.fullscreen_mode == FULLSCREEN_COMPOSITED && !x.unredir_fullscreen,
                  "failed entry disables bypass and remains composited");
            CHECK(!x.pending_reconcile && x.windows[0].imported.image != EGL_NO_IMAGE_KHR,
                  "storage recovery completes before the caller can draw");

            draws = full_clears = swaps = render_op_count = 0;
            x.needs_full_repaint = true;
            CHECK(render_scene(&default_renderer, &x, 1000, 1000),
                  "render recovered scene in main-loop order");
            CHECK(draws == 1 && full_clears == 1,
                  "failed entry still draws the client above the full background");
            CHECK(egl_present(&e) && swaps == 1,
                  "recovered scene is the only frame presented for this iteration");
        }
        x11_release_windows(&x, &e);
    } else if (!strcmp(argv[1], "opacity-threshold")) {
        windows[1].opacity = 0.9995f;
        x11_update_occlusion(&x);
        CHECK(!windows[0].is_occluded, "translucent coverage cannot occlude lower content");
    } else if (!strcmp(argv[1], "opacity-change")) {
        x11_update_occlusion(&x);
        translucent_property = true;
        ev.type = PropertyNotify; ev.xproperty.window = 43; ev.xproperty.atom = 77;
        x11_handle_event(&x, &ev, &dirty, &stop);
        render_scene(&default_renderer, &x, 1000, 1000);
        CHECK(windows[1].opacity < 0.6f && draws == 2, "opacity change reveals lower content");
    } else if (!strcmp(argv[1], "coalesced-damage")) {
        XDamageNotifyEvent *d = (XDamageNotifyEvent *)&ev;
        d->type = 80 + XDamageNotify; d->damage = 200; d->area = (XRectangle){1, 1, 5, 5};
        /* A later server draw extends the accumulated damage beyond the event. */
        damage_bounds = (XRectangle){1, 1, 104, 104};
        x11_handle_event(&x, &ev, &dirty, &stop);
        CHECK(captured_damage != None && covers(&x.client_damage, 104, 104),
              "capture all server damage before clearing it");
        CHECK(!covers(&x.client_damage, 200, 200), "localized damage remains localized");

        damage_list_clear(&x.client_damage);
        x.pending_changes = 0;
        damage_bounds = (XRectangle){8, 9, 20, 21};
        stub_fetch_region_alloc_fail = true;
        dirty = false;
        x11_handle_event(&x, &ev, &dirty, &stop);
        CHECK(dirty && (x.pending_changes & CHANGE_CONTENT),
              "NULL/count-zero fetch with nonempty bounds schedules repaint");
        CHECK(covers(&x.client_damage, 10, 11) && covers(&x.client_damage, 29, 30),
              "allocation failure preserves the fetched damage bounds");

        damage_list_clear(&x.client_damage);
        x.pending_changes = 0;
        stub_fetch_region_alloc_fail = false;
        stub_fetch_region_empty = true;
        dirty = false;
        x11_handle_event(&x, &ev, &dirty, &stop);
        CHECK(!dirty && x.pending_changes == 0 && x.client_damage.count == 0,
              "successful empty fetch creates no repaint work");
    } else if (!strcmp(argv[1], "empty-shape") || !strcmp(argv[1], "shape-uncover")) {
        bool uncover = !strcmp(argv[1], "shape-uncover");
        if (uncover) x11_update_occlusion(&x); else x.num_windows = 1;
        empty_shape = true;
        XShapeEvent *s = (XShapeEvent *)&ev;
        s->type = 90 + ShapeNotify; s->kind = ShapeBounding; s->window = uncover ? 43 : 42;
        x11_handle_event(&x, &ev, &dirty, &stop);
        render_scene(&default_renderer, &x, 1000, 1000);
        CHECK(draws == (uncover ? 1 : 0), "empty shape is invisible and reveals content beneath");
        if (uncover) CHECK(drawn_textures[1] == 1 && drawn_textures[2] == 0,
                           "shape change draws the lower window, not the empty covering window");
    } else if (!strcmp(argv[1], "history")) {
        x.num_windows = 0;
        damage_list_add(&x.client_damage, 10, 10, 5, 5);
        e.display = (EGLDisplay)1; e.surface = (EGLSurface)1; e.has_buffer_age = true;
        RenderState r = test_render_state();
        r.egl = &e;
        render_scene(&r, &x, 1000, 1000);
        CHECK(full_clears == 1, "missing history forces full repaint");
        CHECK(x.history_count > 0, "full repaint seeds history for later partial frames");
        damage_list_add(&x.client_damage, 10, 10, 5, 5);
        render_scene(&r, &x, 1000, 1000);
        damage_list_add(&x.client_damage, 10, 10, 5, 5);
        render_scene(&r, &x, 1000, 1000);
        CHECK(full_clears == 2, "partial repaint resumes once enough history exists");
    } else if (!strcmp(argv[1], "restack")) {
        tree_window_count = 2; tree_windows[0] = 43; tree_windows[1] = 42;
        x11_damage_rect(&x, 900, 900, 1, 1);
        ev.type = CirculateNotify; ev.xcirculate.window = 42;
        x11_handle_event(&x, &ev, &dirty, &stop);
        CHECK(x11_reconcile(&x, &e), "reconcile stacking change");
        CHECK(x.scene_dirty && covers(&x.client_damage, 100, 100), "restack invalidates changed overlap");
    } else if (!strcmp(argv[1], "offscreen-damage-skip")) {
        windows[0].x = -500;
        windows[0].y = -500;
        ev.type = 80 + XDamageNotify;
        XDamageNotifyEvent *d = (XDamageNotifyEvent *)&ev;
        d->damage = 200;
        damage_bounds = (XRectangle){ 0, 0, 100, 100 };
        dirty = false;
        x11_handle_event(&x, &ev, &dirty, &stop);
        CHECK(!dirty, "completely offscreen damage does not set dirty");
        CHECK(x.client_damage.count == 0, "no client damage added");
        render_scene(&default_renderer, &x, 1000, 1000);
        CHECK(draws == 0, "zero draws for offscreen damage");
    } else if (!strcmp(argv[1], "empty-damage-no-swap")) {
        damage_list_clear(&x.client_damage);
        x.needs_full_repaint = false;
        RenderState r = test_render_state();
        r.full_redraw = true;
        CHECK(!render_scene(&r, &x, 1000, 1000),
              "full-redraw mode does not create repaint work");
        CHECK(draws == 0 && swaps == 0,
              "empty damage with no full repaint produces 0 draws and swaps");
    } else if (!strcmp(argv[1], "expose-repair")) {
        x.overlay = 900;
        x.needs_full_repaint = false;
        CHECK(x11_damage_rect(&x, 10, 10, 20, 20), "seed unrelated accumulated damage");
        XEvent expose = {0};
        expose.type = Expose;
        expose.xexpose.window = x.overlay;
        x11_handle_event(&x, &expose, &dirty, &stop);
        CHECK(dirty && x.needs_full_repaint, "overlay exposure schedules a full repair");
        CHECK(x.client_damage.count == 1 && x.client_damage.rects[0].x == 10,
              "exposure keeps previously accumulated damage until rendering");
        RenderState r = test_render_state();
        bool rendered = render_scene(&r, &x, 1000, 1000);
        if (rendered) CHECK(egl_present(&e), "present exposed overlay repair");
        CHECK(rendered && draws > 0 && swaps == 1,
              "exposure causes actual drawing and exactly one swap");
    } else if (!strcmp(argv[1], "hidden-to-visible-repaint")) {
        windows[0].x = -500;
        windows[0].y = -500;
        damage_list_clear(&x.client_damage);
        x.scene_dirty = false;

        tree_window_count = 2;
        tree_windows[0] = 42; tree_windows[1] = 43;
        window_x = 50; window_y = 50;
        client_width = 320; client_height = 240; border_width = 2;

        ev.type = ConfigureNotify;
        ev.xconfigure.window = 42;
        ev.xconfigure.x = 50; ev.xconfigure.y = 50;
        ev.xconfigure.width = 320; ev.xconfigure.height = 240; ev.xconfigure.border_width = 2;
        x11_handle_event(&x, &ev, &dirty, &stop);
        CHECK(x11_reconcile(&x, &e), "reconcile offscreen to onscreen move");
        CHECK(x.scene_dirty, "scene marked dirty when moving into view");
        CHECK(covers(&x.client_damage, 50, 50), "visible footprint damaged");
        RenderState r = test_render_state();
        render_scene(&r, &x, 1000, 1000);
        CHECK(draws > 0, "repainted when moved onscreen");
    } else if (!strcmp(argv[1], "branch-counters")) {
        stats_init(&test_stats);
        x.needs_full_repaint = true;
        RenderState r = test_render_state();
        render_scene(&r, &x, 1000, 1000);
        CHECK(test_stats.full_repaints == 1, "full_repaints incremented on forced full");
        CHECK(!x.needs_full_repaint, "needs_full_repaint cleared after full render");

        e.display = (EGLDisplay)1; e.surface = (EGLSurface)1; e.has_buffer_age = true;
        buffer_age = 1;
        r.egl = &e;
        x11_damage_rect(&x, 10, 10, 20, 20);
        render_scene(&r, &x, 1000, 1000);
        CHECK(test_stats.partial_repaints == 1, "partial_repaints incremented on partial render");
    } else if (!strcmp(argv[1], "shape-clip-restoration")) {
        windows[0].x = 0; windows[0].y = 0;
        windows[0].width = 324; windows[0].height = 244;
        windows[0].border_width = 2;
        windows[0].shaped = true;
        XRectangle *shape_rects = calloc(2, sizeof(XRectangle));
        CHECK(shape_rects != NULL, "allocate shape rects");
        shape_rects[0] = (XRectangle){0, 0, 20, 20};
        shape_rects[1] = (XRectangle){100, 100, 20, 20};
        windows[0].shape_rects = shape_rects;
        windows[0].num_shape_rects = 2;

        windows[1].x = 0; windows[1].y = 0;
        windows[1].width = 324; windows[1].height = 244;
        windows[1].border_width = 2;
        windows[1].shaped = false;

        e.display = (EGLDisplay)1; e.surface = (EGLSurface)1; e.has_buffer_age = true;
        buffer_age = 1;
        RenderState r = test_render_state();
        r.egl = &e;

        x11_damage_rect(&x, 10, 10, 20, 20);
        render_op_count = 0;
        draws = 0;
        bool rendered = render_scene(&r, &x, 1000, 1000);
        CHECK(rendered, "render_scene draws partial frame for buffer age 1");
        CHECK(render_op_count == 3, "expected background clear, shape draw, and window draw");

        /* Assert scissor at background clear */
        CHECK(render_ops[0].op == 'C' && render_ops[0].scissor_enabled &&
              render_ops[0].x == 10 && render_ops[0].y == 970 &&
              render_ops[0].width == 20 && render_ops[0].height == 20,
              "background clear uses outer repair clip (10, 970, 20, 20)");

        /* Assert scissor at shape draw: shape (0,0,20,20) with border 2 has root (2,2,20,20).
         * Intersected with repair (10,10,20,20) -> (10,10,12,12).
         * GL scissor: (10, 1000 - 10 - 12, 12, 12) = (10, 978, 12, 12). */
        CHECK(render_ops[1].op == 'D' && render_ops[1].scissor_enabled &&
              render_ops[1].x == 10 && render_ops[1].y == 978 &&
              render_ops[1].width == 12 && render_ops[1].height == 12,
              "shaped window uses intersected scissor (10, 978, 12, 12)");

        /* Assert scissor at ordinary window draw: outer clip restored */
        CHECK(render_ops[2].op == 'D' && render_ops[2].scissor_enabled &&
              render_ops[2].x == 10 && render_ops[2].y == 970 &&
              render_ops[2].width == 20 && render_ops[2].height == 20,
              "ordinary window afterward uses outer repair clip (10, 970, 20, 20)");

        /* Assert shape outside repair draws nothing */
        CHECK(draws == 2, "shape outside repair draws nothing");

        /* Assert final disabled state */
        CHECK(!scissor_enabled, "scissor test disabled after scene rendering");
    } else if (!strcmp(argv[1], "opaque-blending-bypass")) {
        WindowState test_windows[5];
        memset(test_windows, 0, sizeof(test_windows));
        for (int i = 0; i < 5; i++) {
            test_windows[i] = (WindowState){
                .xid = 50 + i, .viewable = true,
                .x = 10 * i, .y = 10 * i,
                .width = 100, .height = 100,
                .border_width = 0,
                .pixmap = 200 + i, .damage = 300 + i,
                .imported = { .image = (EGLImageKHR)(uintptr_t)(i + 1), .texture = i + 1 }
            };
        }
        test_windows[0].has_alpha = false; test_windows[0].opacity = 1.0f;
        test_windows[1].has_alpha = false; test_windows[1].opacity = 0.5f;
        test_windows[2].has_alpha = true;  test_windows[2].opacity = 1.0f;
        test_windows[3].has_alpha = false; test_windows[3].opacity = 0.9995f;
        test_windows[4].has_alpha = false; test_windows[4].opacity = 1.0f;

        x.windows = test_windows;
        x.num_windows = 5;

        /* Part A: Full render */
        RenderState r = test_render_state();
        r.full_redraw = true;
        x.needs_full_repaint = true;
        render_op_count = 0;
        draws = 0;
        CHECK(render_scene(&r, &x, 1000, 1000), "full scene renders");
        CHECK(draws == 5, "5 window draws issued");
        /* Check blend state at each window draw (skip clear at index 0) */
        CHECK(!render_ops[1].blend_enabled, "w0: opaque RGB has blend disabled");
        CHECK(render_ops[2].blend_enabled,  "w1: translucent RGB has blend enabled");
        CHECK(render_ops[3].blend_enabled,  "w2: ARGB at opacity 1.0 has blend enabled");
        CHECK(render_ops[4].blend_enabled,  "w3: near-one opacity has blend enabled");
        CHECK(!render_ops[5].blend_enabled, "w4: opaque RGB following translucent has blend disabled");

        /* Part B: Partial render with buffer age 1 */
        e.display = (EGLDisplay)1; e.surface = (EGLSurface)1; e.has_buffer_age = true;
        buffer_age = 1;
        r.full_redraw = false;
        r.egl = &e;
        x11_damage_rect(&x, 0, 0, 500, 500);
        render_op_count = 0;
        draws = 0;
        CHECK(render_scene(&r, &x, 1000, 1000), "partial scene renders");
        CHECK(draws == 5, "5 window draws issued in partial render");
        CHECK(!render_ops[1].blend_enabled, "partial w0: opaque RGB has blend disabled");
        CHECK(render_ops[2].blend_enabled,  "partial w1: translucent RGB has blend enabled");
        CHECK(render_ops[3].blend_enabled,  "partial w2: ARGB at opacity 1.0 has blend enabled");
        CHECK(render_ops[4].blend_enabled,  "partial w3: near-one opacity has blend enabled");
        CHECK(!render_ops[5].blend_enabled, "partial w4: opaque RGB has blend disabled");

        x.windows = windows;
        x.num_windows = 2;
    } else if (!strcmp(argv[1], "bounded-presentation-damage")) {
        e.display = (EGLDisplay)1; e.surface = (EGLSurface)1; e.has_buffer_age = true;
        buffer_age = 2;
        RenderState r = test_render_state();
        r.egl = &e;

        /* Frame 1: Seed history with a full frame */
        x.needs_full_repaint = true;
        CHECK(render_scene(&r, &x, 1000, 1000), "seed full frame");
        CHECK(e.presentation_damage_count == 0, "full repaint presentation damage count is 0");

        /* Frame 2: Max-capacity damage (32 rects) */
        damage_list_clear(&x.client_damage);
        for (int i = 0; i < MAX_DAMAGE_RECTS; ++i) {
            damage_list_add(&x.client_damage, i * 10, i * 10, 5, 5);
        }
        CHECK(render_scene(&r, &x, 1000, 1000), "partial frame with 32 rects renders");
        CHECK(e.presentation_damage_count == MAX_DAMAGE_RECTS,
              "presentation_damage_count matches max capacity (32)");
        CHECK(e.presentation_damage[0] == 0 &&
              e.presentation_damage[1] == 995 &&
              e.presentation_damage[2] == 5 &&
              e.presentation_damage[3] == 5,
              "coordinate conversion to EGL bottom-left is correct");

        /* Frame 3: Single damage rect */
        damage_list_clear(&x.client_damage);
        damage_list_add(&x.client_damage, 500, 500, 20, 20);
        CHECK(render_scene(&r, &x, 1000, 1000), "partial frame with 1 rect renders");
        CHECK(e.presentation_damage_count == 1,
              "presentation damage has current frame only (count 1)");
        CHECK(e.presentation_damage[0] == 500 &&
              e.presentation_damage[1] == 1000 - 520 &&
              e.presentation_damage[2] == 20 &&
              e.presentation_damage[3] == 20,
              "current frame damage coordinate conversion matches");
    } else if (!strcmp(argv[1], "static-geometry-reuse")) {
        windows[0].x = 50; windows[0].y = 60;
        windows[0].width = 300; windows[0].height = 200;
        windows[0].border_width = 0;
        windows[0].shaped = false;

        windows[1].x = 100; windows[1].y = 150;
        windows[1].width = 250; windows[1].height = 180;
        windows[1].border_width = 2;
        windows[1].shaped = true;
        XRectangle *shape_rects = calloc(2, sizeof(XRectangle));
        CHECK(shape_rects != NULL, "allocate shape rects");
        shape_rects[0] = (XRectangle){0, 0, 40, 40};
        shape_rects[1] = (XRectangle){60, 60, 40, 40};
        windows[1].shape_rects = shape_rects;
        windows[1].num_shape_rects = 2;

        RenderState r = {0};
        CHECK(render_init(&r, 1000, 1000), "real renderer initialization succeeds offline");
        CHECK(r.vbo == 42 && r.program == 99, "renderer creates its VBO and program");
        CHECK(r.u_resolution_loc == LOC_RESOLUTION && r.u_rect_loc == LOC_RECT &&
              r.u_uv_extent_loc == LOC_UV_EXTENT && r.u_texture_loc == LOC_TEXTURE &&
              r.u_opacity_loc == LOC_OPACITY && r.u_has_alpha_loc == LOC_HAS_ALPHA,
              "initialization resolves distinct uniform locations");
        const float expected_quad[] = {
            0, 0, 0, 0,  1, 0, 1, 0,  0, 1, 0, 1,
            0, 1, 0, 1,  1, 0, 1, 0,  1, 1, 1, 1
        };
        CHECK(stub_buffer_data_calls == 1 && quad_uploaded &&
              !memcmp(uploaded_quad, expected_quad, sizeof(expected_quad)),
              "initialization uploads exactly one complete unit quad with matching UVs");
        CHECK(attributes_enabled[0] && attributes_enabled[1] &&
              attributes_bound[0] && attributes_bound[1],
              "initialization binds position and UV attributes to the uploaded VBO");
        require_initialized_geometry = true;
        x.needs_full_repaint = true;
        stub_buffer_data_calls = 0;
        render_op_count = 0;
        draws = 0;

        CHECK(render_scene(&r, &x, 1000, 1000), "full scene renders");
        CHECK(stub_buffer_data_calls == 0, "strictly 0 vertex buffer uploads during scene rendering");
        /* Expected draws:
         * 1. Window 0 (draws = 1, op 'D'): u_rect = (50, 60, 300, 200), uv = (1.0, 1.0)
         * 2. Window 1 shape rect 0 (draws = 2, op 'D'): u_rect = (100, 150, 250, 180), uv = (1.0, 1.0)
         * 3. Window 1 shape rect 1 (draws = 3, op 'D'): u_rect = (100, 150, 250, 180), uv = (1.0, 1.0)
         */
        CHECK(draws == 3, "three window draws executed (one unshaped + two shape rects)");

        /* Verify distinct window transforms match u_rect */
        CHECK(render_ops[1].op == 'D' &&
              render_ops[1].rect_x == 50.0f && render_ops[1].rect_y == 60.0f &&
              render_ops[1].rect_w == 300.0f && render_ops[1].rect_h == 200.0f &&
              render_ops[1].uv_u == 1.0f && render_ops[1].uv_v == 1.0f,
              "w0 uses distinct rect transform (50, 60, 300, 200) and unit uv");

        /* Verify shaped window draws reuse identical u_rect across distinct shape rect clips */
        CHECK(render_ops[2].op == 'D' &&
              render_ops[2].rect_x == 100.0f && render_ops[2].rect_y == 150.0f &&
              render_ops[2].rect_w == 250.0f && render_ops[2].rect_h == 180.0f &&
              render_ops[2].uv_u == 1.0f && render_ops[2].uv_v == 1.0f,
              "w1 shape clip 0 uses window rect transform (100, 150, 250, 180)");

        CHECK(render_ops[3].op == 'D' &&
              render_ops[3].rect_x == 100.0f && render_ops[3].rect_y == 150.0f &&
              render_ops[3].rect_w == 250.0f && render_ops[3].rect_h == 180.0f &&
              render_ops[3].uv_u == 1.0f && render_ops[3].uv_v == 1.0f,
              "w1 shape clip 1 reuses identical window rect transform (100, 150, 250, 180)");

        /* Verify different scissor clips were applied to each shape rect */
        CHECK(render_ops[2].scissor_enabled && render_ops[3].scissor_enabled,
              "both shape rects drawn with scissor enabled");
        CHECK(render_ops[2].y != render_ops[3].y || render_ops[2].x != render_ops[3].x,
              "shape rects have distinct scissor bounds");

        CHECK(invalid_vertex_state == 0, "all draws use the initialized static geometry");
        CHECK(invalid_uniform_calls == 0, "draw uniforms use the locations resolved at initialization");

        /* Verify teardown releases the static VBO and shader program */
        stub_delete_buffers_calls = 0;
        stub_delete_program_calls = 0;
        render_destroy(&r);
        CHECK(r.vbo == 0 && r.program == 0, "render_destroy resets handles to 0");
        CHECK(stub_delete_buffers_calls == 1 && stub_delete_program_calls == 1,
              "render_destroy deletes VBO and program");

        CHECK(!quad_uploaded, "teardown releases stored geometry");
        CHECK(render_init(&r, 640, 480), "renderer can initialize again after teardown");
        CHECK(stub_buffer_data_calls == 1 && quad_uploaded,
              "reinitialization uploads one fresh quad");
        render_destroy(&r);

        free(shape_rects);
        windows[1].shape_rects = NULL;
        windows[1].num_shape_rects = 0;
        windows[1].shaped = false;
    } else { CHECK(false, "known rendering regression"); }
    CHECK(invalid_uniform_calls == 0, "uniform updates use valid locations and types");
    /* History-only test hides records from rendering, not from cleanup. */
    if (!strcmp(argv[1], "history")) x.num_windows = 2;
    x11_release_windows(&x, &e);
    free(display);
    printf("PASS: %s\n", argv[1]);
    return 0;
}
