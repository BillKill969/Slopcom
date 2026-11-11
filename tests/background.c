#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "x11_stubs.h"
#include "background.h"
#include "render.h"

/* GL Stubs for rendering checks */
static int stub_draw_calls = 0;
static int stub_clear_calls = 0;
static int stub_buffer_data_calls = 0;
static GLuint stub_bound_texture = 0;
static GLint stub_wrap_s, stub_wrap_t;
static float stub_u_extent, stub_v_extent;
static int invalid_uniform_calls;

enum { LOC_RECT = 13, LOC_UV_EXTENT = 17, LOC_OPACITY = 23, LOC_HAS_ALPHA = 29 };

static RenderState test_render_state(void) {
    return (RenderState){
        .u_rect_loc = LOC_RECT, .u_uv_extent_loc = LOC_UV_EXTENT,
        .u_opacity_loc = LOC_OPACITY, .u_has_alpha_loc = LOC_HAS_ALPHA
    };
}

void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { (void)r; (void)g; (void)b; (void)a; }
void glClear(GLbitfield mask) { (void)mask; stub_clear_calls++; }
void glDisable(GLenum cap) { (void)cap; }
void glEnable(GLenum cap) { (void)cap; }
void glBindTexture(GLenum target, GLuint texture) { (void)target; stub_bound_texture = texture; }
void glTexParameteri(GLenum target, GLenum name, GLint value) {
    (void)target;
    if (name == GL_TEXTURE_WRAP_S) stub_wrap_s = value;
    if (name == GL_TEXTURE_WRAP_T) stub_wrap_t = value;
}
void glUniform1f(GLint location, GLfloat v0) {
    (void)v0;
    if (location != LOC_OPACITY) invalid_uniform_calls++;
}
void glUniform1i(GLint location, GLint v0) {
    (void)v0;
    if (location != LOC_HAS_ALPHA) invalid_uniform_calls++;
}
void glUniform2f(GLint location, GLfloat v0, GLfloat v1) {
    if (location != LOC_UV_EXTENT) { invalid_uniform_calls++; return; }
    stub_u_extent = v0;
    stub_v_extent = v1;
}
void glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
    (void)v0; (void)v1; (void)v2; (void)v3;
    if (location != LOC_RECT) invalid_uniform_calls++;
}
void glBindBuffer(GLenum target, GLuint buffer) { (void)target; (void)buffer; }
void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    (void)target; (void)size; (void)data; (void)usage;
    stub_buffer_data_calls++;
}
void glEnableVertexAttribArray(GLuint index) { (void)index; }
void glDisableVertexAttribArray(GLuint index) { (void)index; }
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer) {
    (void)index; (void)size; (void)type; (void)normalized; (void)stride; (void)pointer;
}
void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    (void)mode; (void)first; (void)count;
    stub_draw_calls++;
}

#define CHECK(condition, message) do { if (!(condition)) { \
    fprintf(stderr, "FAIL: %s\n", message); return 1; } } while (0)

int main(int argc, char **argv) {
    CHECK(argc == 2, "select a background test case");

    _XPrivDisplay display_storage = calloc(1, sizeof(*display_storage));
    CHECK(display_storage != NULL, "allocate fake display");
    Display *display = (Display *)display_storage;

    Stats stats;
    stats_init(&stats);
    EGLState e = { .stats = &stats };

    reset_x11_stubs();

    BackgroundState bg;
    background_init(&bg, display, 1, 24);
    CHECK(bg.refresh_required, "init marks refresh required");
    CHECK(!bg.valid, "init starts invalid");
    CHECK(bg.pixmap == None, "init starts with no pixmap");

    if (!strcmp(argv[1], "import-fallback")) {
        stub_primary_prop = (StubProp){ .type = XA_PIXMAP, .format = 32, .count = 1,
            .value = 501, .is_set = true };
        stub_fallback_prop = (StubProp){ .type = XA_PIXMAP, .format = 32, .count = 1,
            .value = 502, .is_set = true };
        stub_geometry_override = true; stub_geometry_width = stub_geometry_height = 100;
        stub_fail_import_pixmap = 501;
        background_refresh(&bg, &e);
        CHECK(bg.valid && bg.pixmap == 502 && imports == 2,
              "import failure on primary tries usable secondary");
        stub_primary_prop.value = stub_fallback_prop.value = 501;
        imports = 0;
        background_refresh(&bg, &e);
        CHECK(!bg.valid && imports == 1, "duplicate failed candidate tried once");
        CHECK(free_pixmap_calls == 0, "never free failed external pixmaps");
    } else if (!strcmp(argv[1], "tiny-tile")) {
        bg.valid = true; bg.width = bg.height = 1; bg.imported.texture = 99;
        RenderState r = test_render_state();
        stub_buffer_data_calls = 0;
        draw_background(&r, &bg, 100, 100);
        CHECK(stub_draw_calls == 1, "tiny power-of-two tile uses one quad");
        CHECK(stub_wrap_s == GL_REPEAT && stub_wrap_t == GL_REPEAT,
              "repeat wrapping enabled only on wallpaper texture");
        CHECK(stub_u_extent == 100 && stub_v_extent == 100, "repeat UVs retain natural pixel size");
        CHECK(stub_buffer_data_calls == 0, "strictly 0 vertex uploads during background drawing");
    } else if (!strcmp(argv[1], "startup-primary")) {
        stub_primary_prop = (StubProp){
            .type = XA_PIXMAP, .format = 32, .count = 1, .remaining = 0,
            .value = 501, .is_set = true
        };
        stub_geometry_override = true;
        stub_geometry_width = 1920;
        stub_geometry_height = 1080;
        stub_geometry_depth = 24;

        background_refresh(&bg, &e);

        CHECK(!bg.refresh_required, "refresh consumed refresh_required flag");
        CHECK(bg.valid, "background is valid after importing primary property");
        CHECK(bg.pixmap == 501, "primary candidate XID imported");
        CHECK(bg.width == 1920 && bg.height == 1080, "geometry dimensions matched");
        CHECK(bg.depth == 24, "depth matched");
        CHECK(imports == 1 && releases == 0, "one import, zero releases");
        CHECK(free_pixmap_calls == 0, "never call XFreePixmap on external wallpaper");
    } else if (!strcmp(argv[1], "fallback-candidate")) {
        /* Primary property absent, fallback present */
        stub_fallback_prop = (StubProp){
            .type = XA_PIXMAP, .format = 32, .count = 1, .remaining = 0,
            .value = 502, .is_set = true
        };
        stub_geometry_override = true;
        stub_geometry_width = 800;
        stub_geometry_height = 600;
        stub_geometry_depth = 24;

        background_refresh(&bg, &e);

        CHECK(!bg.refresh_required, "refresh consumed flag");
        CHECK(bg.valid, "background is valid via fallback atom");
        CHECK(bg.pixmap == 502, "fallback candidate XID imported");
        CHECK(bg.width == 800 && bg.height == 600, "fallback geometry matched");
        CHECK(free_pixmap_calls == 0, "zero XFreePixmap calls");
    } else if (!strcmp(argv[1], "malformed-properties")) {
        /* 1. Wrong type */
        stub_primary_prop = (StubProp){
            .type = XA_CARDINAL, .format = 32, .count = 1, .remaining = 0,
            .value = 503, .is_set = true
        };
        background_refresh(&bg, &e);
        CHECK(!bg.valid, "reject candidate with type != XA_PIXMAP");

        /* 2. Wrong format */
        stub_primary_prop = (StubProp){
            .type = XA_PIXMAP, .format = 16, .count = 1, .remaining = 0,
            .value = 503, .is_set = true
        };
        background_refresh(&bg, &e);
        CHECK(!bg.valid, "reject candidate with format != 32");

        /* 3. Count > 1 */
        stub_primary_prop = (StubProp){
            .type = XA_PIXMAP, .format = 32, .count = 2, .remaining = 0,
            .value = 503, .is_set = true
        };
        background_refresh(&bg, &e);
        CHECK(!bg.valid, "reject candidate with count != 1");

        /* 4. Remaining != 0 */
        stub_primary_prop = (StubProp){
            .type = XA_PIXMAP, .format = 32, .count = 1, .remaining = 4,
            .value = 503, .is_set = true
        };
        background_refresh(&bg, &e);
        CHECK(!bg.valid, "reject candidate with remaining != 0");

        /* 5. Value == None (0) */
        stub_primary_prop = (StubProp){
            .type = XA_PIXMAP, .format = 32, .count = 1, .remaining = 0,
            .value = 0, .is_set = true
        };
        background_refresh(&bg, &e);
        CHECK(!bg.valid, "reject candidate with None value");

        CHECK(free_pixmap_calls == 0, "zero XFreePixmap calls on malformed properties");
    } else if (!strcmp(argv[1], "invalid-geometry")) {
        stub_primary_prop = (StubProp){
            .type = XA_PIXMAP, .format = 32, .count = 1, .remaining = 0,
            .value = 504, .is_set = true
        };
        stub_geometry_override = true;

        /* 1. XGetGeometry returns error */
        stub_geometry_fail = true;
        background_refresh(&bg, &e);
        CHECK(!bg.valid, "XGetGeometry failure causes safe fallback");

        /* 2. Wrong root */
        stub_geometry_fail = false;
        stub_geometry_root = 999;
        stub_geometry_width = 100;
        stub_geometry_height = 100;
        background_refresh(&bg, &e);
        CHECK(!bg.valid, "wrong root window in geometry rejected");

        /* 3. Zero width */
        stub_geometry_root = 1;
        stub_geometry_width = 0;
        stub_geometry_height = 100;
        background_refresh(&bg, &e);
        CHECK(!bg.valid, "zero width rejected");

        /* 4. Unsupported depth */
        stub_geometry_width = 100;
        stub_geometry_height = 100;
        stub_geometry_depth = 8;
        background_refresh(&bg, &e);
        CHECK(!bg.valid, "unsupported depth rejected");

        CHECK(free_pixmap_calls == 0, "zero XFreePixmap calls on invalid geometry");
    } else if (!strcmp(argv[1], "import-failure")) {
        stub_primary_prop = (StubProp){
            .type = XA_PIXMAP, .format = 32, .count = 1, .remaining = 0,
            .value = 505, .is_set = true
        };
        stub_geometry_override = true;
        stub_geometry_width = 100;
        stub_geometry_height = 100;
        stub_geometry_depth = 24;

        import_ok = false;
        background_refresh(&bg, &e);
        CHECK(!bg.valid, "import failure results in fallback");
        CHECK(bg.pixmap == None, "pixmap cleared on import failure");
        CHECK(free_pixmap_calls == 0, "never free external pixmap on import failure");
    } else if (!strcmp(argv[1], "same-xid-refresh")) {
        stub_primary_prop = (StubProp){
            .type = XA_PIXMAP, .format = 32, .count = 1, .remaining = 0,
            .value = 506, .is_set = true
        };
        stub_geometry_override = true;
        stub_geometry_width = 100;
        stub_geometry_height = 100;
        stub_geometry_depth = 24;

        background_refresh(&bg, &e);
        CHECK(bg.valid && imports == 1 && releases == 0, "initial import succeeds");

        /* Re-advertise same XID */
        bg.refresh_required = true;
        background_refresh(&bg, &e);
        CHECK(bg.valid, "background valid after re-importing same XID");
        CHECK(imports == 2, "new import created for same XID update");
        CHECK(releases == 1, "old wrapper retired before re-importing");
        CHECK(free_pixmap_calls == 0, "zero XFreePixmap calls on same-XID refresh");
    } else if (!strcmp(argv[1], "replacement-candidate")) {
        stub_primary_prop = (StubProp){
            .type = XA_PIXMAP, .format = 32, .count = 1, .remaining = 0,
            .value = 507, .is_set = true
        };
        stub_geometry_override = true;
        stub_geometry_width = 100;
        stub_geometry_height = 100;
        stub_geometry_depth = 24;

        background_refresh(&bg, &e);
        CHECK(bg.valid && bg.pixmap == 507, "first wallpaper imported");

        /* Switch to new pixmap */
        stub_primary_prop.value = 508;
        bg.refresh_required = true;
        background_refresh(&bg, &e);
        CHECK(bg.valid && bg.pixmap == 508, "second wallpaper imported");
        CHECK(imports == 2 && releases == 1, "old wrapper released, new wrapper active");
        CHECK(free_pixmap_calls == 0, "zero XFreePixmap calls on candidate replacement");
    } else if (!strcmp(argv[1], "coalesced-events")) {
        X11State x;
        memset(&x, 0, sizeof(x));
        x.display = display;
        x.root = 1;
        x.damage_event_base = 80;
        x.shape_event_base = 90;
        x.net_wm_opacity = 77;
        background_init(&x.background, display, 1, 24);
        x.background.refresh_required = false;

        bool dirty = false, stop = false;
        XEvent ev;
        memset(&ev, 0, sizeof(ev));

        /* 1. PropertyNotify on root with primary atom */
        ev.type = PropertyNotify;
        ev.xproperty.window = 1;
        ev.xproperty.atom = 1001; /* _XROOTPMAP_ID */
        x11_handle_event(&x, &ev, &dirty, &stop);
        CHECK(x.background.refresh_required, "primary wallpaper property triggers refresh");
        CHECK(x.pending_reconcile, "pending reconcile set");

        /* 2. Second PropertyNotify on root with fallback atom */
        ev.xproperty.atom = 1002; /* ESETROOT_PMAP_ID */
        x11_handle_event(&x, &ev, &dirty, &stop);
        CHECK(x.background.refresh_required, "coalesced into pending refresh");

        /* 3. PropertyNotify on non-root window with wallpaper atom should NOT affect background */
        x.background.refresh_required = false;
        ev.xproperty.window = 42;
        ev.xproperty.atom = 1001;
        x11_handle_event(&x, &ev, &dirty, &stop);
        CHECK(!x.background.refresh_required, "non-root window property ignored by background");

        /* 4. PropertyNotify on root with unrelated atom should NOT affect background */
        ev.xproperty.window = 1;
        ev.xproperty.atom = 999;
        x11_handle_event(&x, &ev, &dirty, &stop);
        CHECK(!x.background.refresh_required, "unrelated root property ignored by background");
    } else if (!strcmp(argv[1], "tiling-quads")) {
        /* Screen size: 100x100. Tile size: 37x23.
         * Horizontal tiles: ceil(100/37) = 3 (x=0, 37, 74)
         * Vertical tiles:   ceil(100/23) = 5 (y=0, 23, 46, 69, 92)
         * Total quads: 3 * 5 = 15. */
        bg.valid = true;
        bg.imported.texture = 99;
        bg.width = 37;
        bg.height = 23;

        RenderState r = test_render_state();

        stub_draw_calls = 0;
        stub_clear_calls = 0;
        stub_buffer_data_calls = 0;
        draw_background(&r, &bg, 100, 100);

        CHECK(stub_clear_calls == 1, "clear called once");
        CHECK(stub_bound_texture == 99, "wallpaper texture bound");
        CHECK(stub_draw_calls == 15, "15 quads drawn for 37x23 tile on 100x100 screen");
        CHECK(stub_wrap_s == GL_CLAMP_TO_EDGE && stub_wrap_t == GL_CLAMP_TO_EDGE,
              "GLES2 without NPOT repeat retains legal wrapping");
        CHECK(stub_buffer_data_calls == 0, "strictly 0 vertex uploads during fallback tiling");

        r.has_npot_repeat = true;
        stub_draw_calls = 0;
        stub_buffer_data_calls = 0;
        draw_background(&r, &bg, 100, 100);
        CHECK(stub_draw_calls == 1 && stub_wrap_s == GL_REPEAT && stub_wrap_t == GL_REPEAT,
              "NPOT repeat capability tiles wallpaper in one quad");
        CHECK(stub_u_extent == 100.0f / 37 && stub_v_extent == 100.0f / 23,
              "NPOT repeat retains natural size and fractional edge tiles");
        CHECK(stub_buffer_data_calls == 0, "strictly 0 vertex uploads during NPOT repeat tiling");

        /* When bg is invalid, only clear occurs, 0 draw calls */
        bg.valid = false;
        stub_draw_calls = 0;
        stub_clear_calls = 0;
        draw_background(&r, &bg, 100, 100);
        CHECK(stub_clear_calls == 1, "fallback clear called");
        CHECK(stub_draw_calls == 0, "no quads drawn for invalid background");
    } else if (!strcmp(argv[1], "repeated-release")) {
        stub_primary_prop = (StubProp){
            .type = XA_PIXMAP, .format = 32, .count = 1, .remaining = 0,
            .value = 509, .is_set = true
        };
        stub_geometry_override = true;
        stub_geometry_width = 100;
        stub_geometry_height = 100;
        stub_geometry_depth = 24;

        background_refresh(&bg, &e);
        CHECK(bg.valid && imports == 1 && releases == 0, "imported");

        background_release(&bg, &e);
        CHECK(!bg.valid && releases == 1, "first release succeeds");
        CHECK(bg.pixmap == None && bg.width == 0 && bg.height == 0, "state cleared");

        background_release(&bg, &e);
        CHECK(!bg.valid && releases == 1, "second release is idempotent");
        CHECK(free_pixmap_calls == 0, "zero XFreePixmap calls on repeated release");
    } else if (!strcmp(argv[1], "zero-external-free")) {
        CHECK(free_pixmap_calls == 0, "strictly zero external XFreePixmap calls");
    } else {
        CHECK(false, "unknown test case");
    }

    CHECK(invalid_uniform_calls == 0, "background uniforms use valid locations and types");
    background_release(&bg, &e);
    free(display_storage);
    printf("PASS: %s\n", argv[1]);
    return 0;
}
