#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <X11/Xlib.h>

typedef struct {
    EGLint renderable;
    EGLint surface_type;
    EGLint visual_id;
    EGLint min_interval;
    EGLint max_interval;
    EGLint samples;
    EGLint depth;
    EGLint stencil;
    bool fail_attrib;
    EGLint fail_attrib_name;
} MockConfig;

#define MAX_MOCK_CONFIGS 16
static MockConfig mock_configs[MAX_MOCK_CONFIGS];
static int num_mock_configs = 0;

/* Stub eglGetConfigAttrib to feed synthetic attributes to selection */
EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value) {
    (void)dpy;
    int idx = (int)(uintptr_t)config - 1;
    if (idx < 0 || idx >= num_mock_configs) return EGL_FALSE;
    MockConfig *mc = &mock_configs[idx];
    if (mc->fail_attrib && mc->fail_attrib_name == attribute) {
        return EGL_FALSE;
    }
    switch (attribute) {
        case EGL_RENDERABLE_TYPE:    *value = mc->renderable; return EGL_TRUE;
        case EGL_SURFACE_TYPE:       *value = mc->surface_type; return EGL_TRUE;
        case EGL_NATIVE_VISUAL_ID:   *value = mc->visual_id; return EGL_TRUE;
        case EGL_MIN_SWAP_INTERVAL:  *value = mc->min_interval; return EGL_TRUE;
        case EGL_MAX_SWAP_INTERVAL:  *value = mc->max_interval; return EGL_TRUE;
        case EGL_SAMPLES:            *value = mc->samples; return EGL_TRUE;
        case EGL_DEPTH_SIZE:         *value = mc->depth; return EGL_TRUE;
        case EGL_STENCIL_SIZE:       *value = mc->stencil; return EGL_TRUE;
        default:                     *value = 0; return EGL_TRUE;
    }
}

/* Include implementation to test private static selection helpers directly */
#include "../src/egl.c"

static GLuint generated_texture_name;
static GLenum pending_gl_error;
static unsigned int texture_generations;
static unsigned int texture_binds;
static unsigned int zero_texture_binds;
static unsigned int texture_deletions;
static unsigned int image_creations;
static unsigned int image_destructions;
static unsigned int image_bindings;

EGLImageKHR mock_create_image(EGLDisplay display, EGLContext context, EGLenum target,
                              EGLClientBuffer buffer, const EGLint *attributes) {
    (void)display;
    (void)context;
    (void)target;
    (void)buffer;
    (void)attributes;
    image_creations++;
    return (EGLImageKHR)(uintptr_t)0x42;
}

EGLBoolean mock_destroy_image(EGLDisplay display, EGLImageKHR image) {
    (void)display;
    (void)image;
    image_destructions++;
    return EGL_TRUE;
}

void mock_bind_image(GLenum target, GLeglImageOES image) {
    (void)target;
    (void)image;
    image_bindings++;
}

void GL_APIENTRY glGenTextures(GLsizei count, GLuint *textures) {
    texture_generations++;
    if (count > 0) textures[0] = generated_texture_name;
}

void GL_APIENTRY glBindTexture(GLenum target, GLuint texture) {
    (void)target;
    texture_binds++;
    if (texture == 0) zero_texture_binds++;
}

GLenum GL_APIENTRY glGetError(void) {
    GLenum error = pending_gl_error;
    pending_gl_error = GL_NO_ERROR;
    return error;
}

void GL_APIENTRY glTexParameteri(GLenum target, GLenum parameter, GLint value) {
    (void)target;
    (void)parameter;
    (void)value;
}

void GL_APIENTRY glDeleteTextures(GLsizei count, const GLuint *textures) {
    if (count > 0 && textures[0] != 0) texture_deletions++;
}

#define CHECK(condition, message) do { if (!(condition)) { \
    fprintf(stderr, "FAIL: %s\n", message); return 1; } } while (0)

static void reset_mock_configs(void) {
    memset(mock_configs, 0, sizeof(mock_configs));
    num_mock_configs = 0;
}

static EGLConfig add_mock_config(MockConfig cfg) {
    assert(num_mock_configs < MAX_MOCK_CONFIGS);
    mock_configs[num_mock_configs] = cfg;
    num_mock_configs++;
    return (EGLConfig)(uintptr_t)num_mock_configs;
}

static int test_import_texture_accounting(void) {
    Stats stats = {0};
    EGLState egl = {
        .display = (EGLDisplay)(uintptr_t)1,
        .create_image = mock_create_image,
        .destroy_image = mock_destroy_image,
        .bind_image = mock_bind_image,
        .stats = &stats
    };
    ImportedPixmap imported;

    generated_texture_name = 0;
    pending_gl_error = GL_OUT_OF_MEMORY;
    CHECK(!egl_import_pixmap(&egl, (Pixmap)0x99, &imported),
          "zero texture name fails pixmap import");
    CHECK(imported.image == EGL_NO_IMAGE_KHR && imported.texture == 0,
          "failed import clears image and texture handles");
    CHECK(stats.live_images == 0 && stats.live_textures == 0,
          "failed texture generation restores image and texture counters");
    CHECK(image_creations == 1 && image_destructions == 1,
          "failed texture generation destroys its created image");
    CHECK(texture_binds == 0 && image_bindings == 0 && zero_texture_binds == 0,
          "failed texture generation does not bind texture zero or the image");

    generated_texture_name = 7;
    pending_gl_error = GL_NO_ERROR;
    CHECK(egl_import_pixmap(&egl, (Pixmap)0x9a, &imported),
          "valid texture name imports pixmap");
    CHECK(imported.texture == 7 && stats.live_images == 1 && stats.live_textures == 1,
          "successful import increments both live-resource counters");
    CHECK(texture_binds == 1 && image_bindings == 1 && zero_texture_binds == 0,
          "successful import binds the generated texture and image");

    egl_release_import(&egl, &imported);
    CHECK(imported.image == EGL_NO_IMAGE_KHR && imported.texture == 0,
          "release clears imported handles");
    CHECK(stats.live_images == 0 && stats.live_textures == 0,
          "release restores both live-resource counters");
    CHECK(texture_deletions == 1 && image_destructions == 2,
          "release deletes the valid texture and destroys its image");
    return 0;
}

int main(void) {
    VisualID target_visual = 0x21;
    EGLDisplay fake_dpy = (EGLDisplay)(uintptr_t)1;

    /* Base valid config template */
    MockConfig base_valid = {
        .renderable = EGL_OPENGL_ES2_BIT,
        .surface_type = EGL_WINDOW_BIT,
        .visual_id = 0x21,
        .min_interval = 0,
        .max_interval = 1,
        .samples = 0,
        .depth = 0,
        .stencil = 0
    };

    /* 1. Incompatible visual ID */
    {
        reset_mock_configs();
        MockConfig bad_vis = base_valid;
        bad_vis.visual_id = 0x99;
        EGLConfig c = add_mock_config(bad_vis);
        ConfigScore score;
        CHECK(!egl_is_config_eligible(fake_dpy, c, target_visual, 1, &score),
              "incompatible visual ID rejected");
    }

    /* 2. Incompatible renderability (missing GLES2) */
    {
        reset_mock_configs();
        MockConfig no_gles2 = base_valid;
        no_gles2.renderable = 0;
        EGLConfig c = add_mock_config(no_gles2);
        CHECK(!egl_is_config_eligible(fake_dpy, c, target_visual, 1, NULL),
              "missing GLES2 renderable bit rejected");
    }

    /* 3. Incompatible surface type (missing EGL_WINDOW_BIT) */
    {
        reset_mock_configs();
        MockConfig no_window = base_valid;
        no_window.surface_type = EGL_PBUFFER_BIT;
        EGLConfig c = add_mock_config(no_window);
        CHECK(!egl_is_config_eligible(fake_dpy, c, target_visual, 1, NULL),
              "missing window surface bit rejected");
    }

    /* 4. Failed attribute query return values */
    {
        reset_mock_configs();
        MockConfig fail_query = base_valid;
        fail_query.fail_attrib = true;
        fail_query.fail_attrib_name = EGL_DEPTH_SIZE;
        EGLConfig c = add_mock_config(fail_query);
        CHECK(!egl_is_config_eligible(fake_dpy, c, target_visual, 1, NULL),
              "failed attribute query rejected");
    }

    /* 5. Swap interval range rejection */
    {
        reset_mock_configs();
        MockConfig no_vsync_support = base_valid;
        no_vsync_support.min_interval = 0;
        no_vsync_support.max_interval = 0; /* Cannot support interval 1 */
        EGLConfig c = add_mock_config(no_vsync_support);
        CHECK(!egl_is_config_eligible(fake_dpy, c, target_visual, 1, NULL),
              "requested interval 1 above max_interval rejected");
        CHECK(egl_is_config_eligible(fake_dpy, c, target_visual, 0, NULL),
              "requested interval 0 within range accepted");
    }

    /* 6. A later better candidate is selected */
    {
        reset_mock_configs();
        MockConfig c1_bloated = base_valid;
        c1_bloated.depth = 24; c1_bloated.stencil = 8; c1_bloated.samples = 4;
        MockConfig c2_perfect = base_valid;
        c2_perfect.depth = 0; c2_perfect.stencil = 0; c2_perfect.samples = 0;

        EGLConfig configs[2];
        configs[0] = add_mock_config(c1_bloated);
        configs[1] = add_mock_config(c2_perfect);

        ConfigScore score;
        EGLConfig chosen = egl_select_best_config(fake_dpy, configs, 2, target_visual, 1, &score);
        CHECK(chosen == configs[1], "later better zero-extra candidate selected over earlier bloated config");
        CHECK(score.depth == 0 && score.stencil == 0 && score.samples == 0, "chosen score is 0/0/0");
    }

    /* 7. Fallback ranking when zero-extra configs are absent:
     * Deterministic order: multisampling (samples), then depth, then stencil. */
    {
        reset_mock_configs();
        MockConfig c1_msaa = base_valid;
        c1_msaa.samples = 4; c1_msaa.depth = 16; c1_msaa.stencil = 0;

        MockConfig c2_depth24 = base_valid;
        c2_depth24.samples = 0; c2_depth24.depth = 24; c2_depth24.stencil = 8;

        MockConfig c3_depth16 = base_valid;
        c3_depth16.samples = 0; c3_depth16.depth = 16; c3_depth16.stencil = 8;

        EGLConfig configs[3];
        configs[0] = add_mock_config(c1_msaa);
        configs[1] = add_mock_config(c2_depth24);
        configs[2] = add_mock_config(c3_depth16);

        ConfigScore score;
        EGLConfig chosen = egl_select_best_config(fake_dpy, configs, 3, target_visual, 1, &score);
        CHECK(chosen == configs[2], "samples=0 beats samples=4, and depth=16 beats depth=24");
        CHECK(score.samples == 0 && score.depth == 16 && score.stencil == 8,
              "fallback score reflects best eligible config");
    }

    /* 8. Tie-breaking retains first candidate */
    {
        reset_mock_configs();
        MockConfig c1_tie = base_valid;
        c1_tie.depth = 16; c1_tie.stencil = 0; c1_tie.samples = 0;
        MockConfig c2_tie = base_valid;
        c2_tie.depth = 16; c2_tie.stencil = 0; c2_tie.samples = 0;

        EGLConfig configs[2];
        configs[0] = add_mock_config(c1_tie);
        configs[1] = add_mock_config(c2_tie);

        ConfigScore score;
        EGLConfig chosen = egl_select_best_config(fake_dpy, configs, 2, target_visual, 1, &score);
        CHECK(chosen == configs[0], "tie-breaking retains first candidate");
    }

    /* 9. No eligible config */
    {
        reset_mock_configs();
        MockConfig bad_vis = base_valid;
        bad_vis.visual_id = 0x999;
        EGLConfig configs[1];
        configs[0] = add_mock_config(bad_vis);

        ConfigScore score;
        EGLConfig chosen = egl_select_best_config(fake_dpy, configs, 1, target_visual, 1, &score);
        CHECK(chosen == NULL, "returns NULL when no eligible config exists");
    }

    if (test_import_texture_accounting() != 0) return 1;

    printf("PASS: egl-config-selection-and-import-accounting\n");
    return 0;
}
