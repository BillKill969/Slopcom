#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <GLES2/gl2.h>
#include <X11/extensions/Xfixes.h>

#include "render.h"
#include "x11.h"
#include "stats.h"

static const char *k_vertex_shader_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "uniform vec2 u_resolution;\n"
    "uniform vec4 u_rect;\n"
    "uniform vec2 u_uv_extent;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 pixel = u_rect.xy + a_pos * u_rect.zw;\n"
    "    float x_ndc = (2.0 * pixel.x / u_resolution.x) - 1.0;\n"
    "    float y_ndc = 1.0 - (2.0 * pixel.y / u_resolution.y);\n"
    "    gl_Position = vec4(x_ndc, y_ndc, 0.0, 1.0);\n"
    "    v_uv = a_uv * u_uv_extent;\n"
    "}\n";

static const char *k_fragment_shader_src =
    "precision mediump float;\n"
    "uniform sampler2D u_texture;\n"
    "uniform float u_opacity;\n"
    "uniform bool u_has_alpha;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    vec4 color = texture2D(u_texture, v_uv);\n"
    "    if (!u_has_alpha) color.a = 1.0;\n"
    "    gl_FragColor = color * u_opacity;\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;

    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint info_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len > 1) {
            char *info_log = malloc(info_len);
            if (info_log) {
                glGetShaderInfoLog(shader, info_len, NULL, info_log);
                fprintf(stderr, "slopcom: Shader compilation error: %s\n", info_log);
                free(info_log);
            }
        }
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

typedef struct {
    float x, y;
    float u, v;
} UnitVertex;

static const UnitVertex k_unit_quad[6] = {
    { 0.0f, 0.0f, 0.0f, 0.0f },
    { 1.0f, 0.0f, 1.0f, 0.0f },
    { 0.0f, 1.0f, 0.0f, 1.0f },
    { 0.0f, 1.0f, 0.0f, 1.0f },
    { 1.0f, 0.0f, 1.0f, 0.0f },
    { 1.0f, 1.0f, 1.0f, 1.0f },
};

static void render_bind_quad_state(RenderState *r) {
    if (r->vbo != 0) {
        glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(UnitVertex), (void *)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(UnitVertex), (void *)(2 * sizeof(float)));
    }
}

bool render_init(RenderState *r, int width, int height) {
    bool saved_full_redraw = r->full_redraw;
    memset(r, 0, sizeof(*r));
    r->full_redraw = saved_full_redraw;

    const char *version = (const char *)glGetString(GL_VERSION);
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    int major = 0;
    if (version) sscanf(version, "OpenGL ES %d", &major);
    r->has_npot_repeat = major >= 3;
    const char *ext = extensions;
    const char *npot = "GL_OES_texture_npot";
    while (ext && (ext = strstr(ext, npot))) {
        size_t len = strlen(npot);
        if ((ext == extensions || ext[-1] == ' ') &&
            (ext[len] == '\0' || ext[len] == ' ')) {
            r->has_npot_repeat = true;
            break;
        }
        ext += len;
    }

    GLuint vs = compile_shader(GL_VERTEX_SHADER, k_vertex_shader_src);
    if (!vs) return false;

    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, k_fragment_shader_src);
    if (!fs) {
        glDeleteShader(vs);
        return false;
    }

    r->program = glCreateProgram();
    if (!r->program) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    glAttachShader(r->program, vs);
    glAttachShader(r->program, fs);
    glBindAttribLocation(r->program, 0, "a_pos");
    glBindAttribLocation(r->program, 1, "a_uv");
    glLinkProgram(r->program);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(r->program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint info_len = 0;
        glGetProgramiv(r->program, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len > 1) {
            char *info_log = malloc(info_len);
            if (info_log) {
                glGetProgramInfoLog(r->program, info_len, NULL, info_log);
                fprintf(stderr, "slopcom: Program link error: %s\n", info_log);
                free(info_log);
            }
        }
        glDeleteProgram(r->program);
        r->program = 0;
        return false;
    }

    r->u_resolution_loc = glGetUniformLocation(r->program, "u_resolution");
    r->u_rect_loc = glGetUniformLocation(r->program, "u_rect");
    r->u_uv_extent_loc = glGetUniformLocation(r->program, "u_uv_extent");
    r->u_texture_loc = glGetUniformLocation(r->program, "u_texture");
    r->u_opacity_loc = glGetUniformLocation(r->program, "u_opacity");
    r->u_has_alpha_loc = glGetUniformLocation(r->program, "u_has_alpha");

    glGenBuffers(1, &r->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(k_unit_quad), k_unit_quad, GL_STATIC_DRAW);
    render_bind_quad_state(r);

    glViewport(0, 0, width, height);
    return true;
}

static void draw_quad_uv(RenderState *r, float x, float y, float w, float h, float u, float v) {
    glUniform4f(r->u_rect_loc, x, y, w, h);
    glUniform2f(r->u_uv_extent_loc, u, v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void draw_quad(RenderState *r, float x, float y, float w, float h) {
    draw_quad_uv(r, x, y, w, h, 1.0f, 1.0f);
}

static void draw_wallpaper_quads(RenderState *r, BackgroundState *bg, int width, int height) {
    if (!bg || bg->width == 0 || bg->height == 0) return;
    bool power_of_two = (bg->width & (bg->width - 1)) == 0 &&
        (bg->height & (bg->height - 1)) == 0;
    bool repeat = power_of_two || r->has_npot_repeat;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    if (repeat) {
        draw_quad_uv(r, 0, 0, (float)width, (float)height,
            (float)width / bg->width, (float)height / bg->height);
        return;
    }
    for (int y = 0; y < height; y += (int)bg->height) {
        for (int x = 0; x < width; x += (int)bg->width) {
            draw_quad(r, (float)x, (float)y, (float)bg->width, (float)bg->height);
        }
    }
}

void draw_background(RenderState *r, BackgroundState *bg, int width, int height) {
    glClearColor(0.12f, 0.14f, 0.17f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (bg && bg->valid) {
        render_bind_quad_state(r);
        glDisable(GL_BLEND);
        glBindTexture(GL_TEXTURE_2D, bg->imported.texture);
        glUniform1f(r->u_opacity_loc, 1.0f);
        glUniform1i(r->u_has_alpha_loc, 0);
        draw_wallpaper_quads(r, bg, width, height);
        glEnable(GL_BLEND);
    }
}

static void set_scissor(const DamageRect *clip, int screen_height) {
    glScissor(clip->x, screen_height - clip->y - clip->height,
              clip->width, clip->height);
}

/* Private helper interface; callers pass a nonempty viewport-clipped region. */
static void draw_scene_windows(RenderState *r, X11State *x,
                               const DamageRect *clip, int screen_height,
                               bool outer_scissored) {
    for (size_t i = 0; i < x->num_windows; ++i) {
        WindowState *w = &x->windows[i];
        if (!w->viewable || w->imported.texture == 0 || (w->is_occluded && !r->full_redraw)) {
            continue;
        }
        if (w->shaped && (!w->shape_rects || w->num_shape_rects == 0)) continue;

        DamageRect win_rect = { w->x, w->y, w->width, w->height };
        DamageRect win_clip;
        if (!damage_rect_intersect(&win_rect, clip, &win_clip)) {
            continue;
        }

        bool opaque = !w->has_alpha && w->opacity == 1.0f;
        if (opaque) {
            glDisable(GL_BLEND);
        } else {
            glEnable(GL_BLEND);
        }

        glBindTexture(GL_TEXTURE_2D, w->imported.texture);
        glUniform1f(r->u_opacity_loc, w->opacity);
        glUniform1i(r->u_has_alpha_loc, w->has_alpha ? 1 : 0);

        if (w->shaped && w->shape_rects && w->num_shape_rects > 0) {
            if (!outer_scissored) glEnable(GL_SCISSOR_TEST);
            for (int s_i = 0; s_i < w->num_shape_rects; ++s_i) {
                XRectangle *sr = &w->shape_rects[s_i];
                DamageRect shape_rect = {
                    w->x + sr->x + w->border_width,
                    w->y + sr->y + w->border_width,
                    sr->width,
                    sr->height
                };
                DamageRect intersection;
                if (!damage_rect_intersect(&shape_rect, clip, &intersection)) continue;
                set_scissor(&intersection, screen_height);
                draw_quad(r, (float)w->x, (float)w->y, (float)w->width, (float)w->height);
            }
            if (outer_scissored) set_scissor(clip, screen_height);
            else glDisable(GL_SCISSOR_TEST);
        } else {
            draw_quad(r, (float)w->x, (float)w->y, (float)w->width, (float)w->height);
        }
    }
    glEnable(GL_BLEND);
}

static void record_frame_damage(X11State *x, const DamageList *damage) {
    x->client_history[x->history_head] = *damage;
    x->history_head = (x->history_head + 1) % DAMAGE_HISTORY_CAP;
    if (x->history_count < DAMAGE_HISTORY_CAP) x->history_count++;
    damage_list_clear(&x->client_damage);
}

static void render_full(RenderState *r, X11State *x, int width, int height) {
    glDisable(GL_SCISSOR_TEST);

    glUseProgram(r->program);
    glUniform2f(r->u_resolution_loc, (float)width, (float)height);
    render_bind_quad_state(r);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glActiveTexture(GL_TEXTURE0);
    glUniform1i(r->u_texture_loc, 0);

    draw_background(r, &x->background, width, height);

    DamageRect viewport = {0, 0, width, height};
    draw_scene_windows(r, x, &viewport, height, false);

    /* A complete frame establishes valid history instead of preventing partial
     * rendering from ever starting on double/triple-buffered surfaces. */
    DamageList full = { .rects = {{0, 0, width, height}}, .count = 1 };
    record_frame_damage(x, &full);

    if (r->egl) {
        r->egl->presentation_damage_count = 0;
    }
}

bool render_scene(RenderState *r, X11State *x, int width, int height) {
    if (!x11_repaint_pending(x)) {
        return false;
    }

    glViewport(0, 0, width, height);

    /* --full-redraw selects the drawing mode for requested work; it does not
     * create repaint work on its own. */
    bool force_full = r->full_redraw || x->needs_full_repaint;

    EGLState *e = r->egl;
    EGLint age = 0;
    if (!force_full && e && e->has_buffer_age && e->display != EGL_NO_DISPLAY && e->surface != EGL_NO_SURFACE) {
        if (!eglQuerySurface(e->display, e->surface, EGL_BUFFER_AGE_EXT, &age)) {
            age = 0;
        }
    }
    if (!e || !e->has_buffer_age || age <= 0 || age > DAMAGE_HISTORY_CAP || age - 1 > x->history_count) {
        force_full = true;
    }

    if (force_full) {
        x->needs_full_repaint = false;
        if (x->stats) x->stats->full_repaints++;
        render_full(r, x, width, height);
        return true;
    }

    /* Valid buffer age (1 <= age <= DAMAGE_HISTORY_CAP): build repaint list */
    DamageList repaint_list = x->client_damage;
    for (int i = 1; i < age; ++i) {
        int idx = (x->history_head - i + DAMAGE_HISTORY_CAP) % DAMAGE_HISTORY_CAP;
        damage_list_union(&repaint_list, &x->client_history[idx]);
    }

    if (repaint_list.count == 0) {
        x->needs_full_repaint = false;
        if (x->stats) x->stats->full_repaints++;
        render_full(r, x, width, height);
        return true;
    }

    if (x->stats) x->stats->partial_repaints++;

    /* Set partial update damage region if supported */
    if (e->has_partial_update && e->set_damage_region) {
        EGLint egl_repaint[MAX_DAMAGE_RECTS * 4];
        for (int i = 0; i < repaint_list.count; ++i) {
            egl_repaint[4 * i + 0] = repaint_list.rects[i].x;
            egl_repaint[4 * i + 1] = height - (repaint_list.rects[i].y + repaint_list.rects[i].height);
            egl_repaint[4 * i + 2] = repaint_list.rects[i].width;
            egl_repaint[4 * i + 3] = repaint_list.rects[i].height;
        }
        e->set_damage_region(e->display, e->surface, egl_repaint, repaint_list.count);
    }

    /* Fetch current presentation damage (frame N changes only) */
    if (x->client_damage.count > 0 && x->client_damage.count <= MAX_DAMAGE_RECTS) {
        for (int i = 0; i < x->client_damage.count; ++i) {
            e->presentation_damage[4 * i + 0] = x->client_damage.rects[i].x;
            e->presentation_damage[4 * i + 1] = height - (x->client_damage.rects[i].y + x->client_damage.rects[i].height);
            e->presentation_damage[4 * i + 2] = x->client_damage.rects[i].width;
            e->presentation_damage[4 * i + 3] = x->client_damage.rects[i].height;
        }
        e->presentation_damage_count = x->client_damage.count;
    } else {
        e->presentation_damage_count = 0;
    }

    DamageList frame_damage = x->client_damage;
    record_frame_damage(x, &frame_damage);

    /* Render damaged regions */
    glEnable(GL_SCISSOR_TEST);
    glUseProgram(r->program);
    glUniform2f(r->u_resolution_loc, (float)width, (float)height);
    render_bind_quad_state(r);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(r->u_texture_loc, 0);

    DamageRect viewport = {0, 0, width, height};
    for (int k = 0; k < repaint_list.count; ++k) {
        DamageRect clip;
        if (!damage_rect_intersect(&repaint_list.rects[k], &viewport, &clip)) continue;

        set_scissor(&clip, height);
        draw_background(r, &x->background, width, height);
        draw_scene_windows(r, x, &clip, height, true);
    }

    glDisable(GL_SCISSOR_TEST);
    return true;
}

void render_destroy(RenderState *r) {
    if (r->vbo != 0) {
        glDeleteBuffers(1, &r->vbo);
        r->vbo = 0;
    }
    if (r->program != 0) {
        glDeleteProgram(r->program);
        r->program = 0;
    }
}
