#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "egl.h"
#include "stats.h"

#ifndef EGL_PLATFORM_X11_EXT
#define EGL_PLATFORM_X11_EXT 0x31D5
#endif

typedef struct {
    EGLint samples;
    EGLint depth;
    EGLint stencil;
} ConfigScore;

static bool egl_is_config_eligible(EGLDisplay dpy, EGLConfig cfg,
                                   VisualID overlay_visual_id, int requested_interval,
                                   ConfigScore *out_score) {
    EGLint renderable = 0, surface_type = 0, visual_id = 0;
    if (!eglGetConfigAttrib(dpy, cfg, EGL_RENDERABLE_TYPE, &renderable)) return false;
    if (!(renderable & EGL_OPENGL_ES2_BIT)) return false;

    if (!eglGetConfigAttrib(dpy, cfg, EGL_SURFACE_TYPE, &surface_type)) return false;
    if (!(surface_type & EGL_WINDOW_BIT)) return false;

    if (!eglGetConfigAttrib(dpy, cfg, EGL_NATIVE_VISUAL_ID, &visual_id)) return false;
    if ((VisualID)visual_id != overlay_visual_id) return false;

    EGLint min_interval = 0, max_interval = 0;
    if (!eglGetConfigAttrib(dpy, cfg, EGL_MIN_SWAP_INTERVAL, &min_interval)) return false;
    if (!eglGetConfigAttrib(dpy, cfg, EGL_MAX_SWAP_INTERVAL, &max_interval)) return false;
    if (requested_interval < min_interval || requested_interval > max_interval) return false;

    EGLint samples = 0, depth = 0, stencil = 0;
    if (!eglGetConfigAttrib(dpy, cfg, EGL_SAMPLES, &samples)) return false;
    if (!eglGetConfigAttrib(dpy, cfg, EGL_DEPTH_SIZE, &depth)) return false;
    if (!eglGetConfigAttrib(dpy, cfg, EGL_STENCIL_SIZE, &stencil)) return false;

    if (out_score) {
        out_score->samples = samples;
        out_score->depth = depth;
        out_score->stencil = stencil;
    }
    return true;
}

static EGLConfig egl_select_best_config(EGLDisplay dpy, EGLConfig *configs, int num_configs,
                                        VisualID overlay_visual_id, int requested_interval,
                                        ConfigScore *out_score) {
    EGLConfig best_cfg = NULL;
    ConfigScore best_score = { 9999, 9999, 9999 };

    for (int i = 0; i < num_configs; ++i) {
        ConfigScore score;
        if (!egl_is_config_eligible(dpy, configs[i], overlay_visual_id, requested_interval, &score)) {
            continue;
        }

        if (!best_cfg) {
            best_cfg = configs[i];
            best_score = score;
            if (best_score.samples == 0 && best_score.depth == 0 && best_score.stencil == 0) {
                break; /* Perfect zero-extra match, first candidate retained */
            }
            continue;
        }

        /* Deterministic ranking: multisampling (samples), then depth, then stencil.
         * Retaining first candidate on ties. */
        bool better = false;
        if (score.samples < best_score.samples) {
            better = true;
        } else if (score.samples == best_score.samples) {
            if (score.depth < best_score.depth) {
                better = true;
            } else if (score.depth == best_score.depth) {
                if (score.stencil < best_score.stencil) {
                    better = true;
                }
            }
        }

        if (better) {
            best_cfg = configs[i];
            best_score = score;
            if (best_score.samples == 0 && best_score.depth == 0 && best_score.stencil == 0) {
                break; /* Cannot beat 0/0/0 */
            }
        }
    }

    if (out_score && best_cfg) {
        *out_score = best_score;
    }
    return best_cfg;
}

bool egl_init(EGLState *e, Display *xdisplay, Window overlay, VisualID overlay_visual_id, int width, int height) {
    Stats *saved_stats = e->stats;
    bool saved_full_redraw = e->full_redraw;
    bool saved_no_vsync = e->no_vsync;
    memset(e, 0, sizeof(*e));
    e->stats = saved_stats;
    e->full_redraw = saved_full_redraw;
    e->no_vsync = saved_no_vsync;
    e->display = EGL_NO_DISPLAY;
    e->surface = EGL_NO_SURFACE;
    e->context = EGL_NO_CONTEXT;

    /* Check client extensions for platform EXT support */
    const char *client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    bool has_platform_base = client_exts && strstr(client_exts, "EGL_EXT_platform_base") != NULL;
    bool has_platform_x11 = client_exts && (strstr(client_exts, "EGL_EXT_platform_x11") != NULL ||
                                             strstr(client_exts, "EGL_KHR_platform_x11") != NULL);

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = NULL;
    PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC create_platform_window_surface = NULL;

    if (has_platform_base && has_platform_x11) {
        get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        create_platform_window_surface = (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
        if (get_platform_display && create_platform_window_surface) {
            e->is_platform_ext = true;
            e->display = get_platform_display(EGL_PLATFORM_X11_EXT, (void *)xdisplay, NULL);
        }
    }

    if (e->display == EGL_NO_DISPLAY) {
        e->is_platform_ext = false;
        e->display = eglGetDisplay((EGLNativeDisplayType)xdisplay);
    }

    if (e->display == EGL_NO_DISPLAY) {
        fprintf(stderr, "slopcom: Failed to obtain EGL display\n");
        return false;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(e->display, &major, &minor)) {
        fprintf(stderr, "slopcom: eglInitialize failed: 0x%x\n", eglGetError());
        goto fail;
    }
    e->is_initialized = true;

    /* Bind OpenGL ES API */
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "slopcom: Failed to bind OpenGL ES API: 0x%x\n", eglGetError());
        goto fail;
    }

    /* Enumerate configs to match overlay visual ID */
    EGLint num_configs = 0;
    if (!eglGetConfigs(e->display, NULL, 0, &num_configs) || num_configs <= 0) {
        fprintf(stderr, "slopcom: No EGL configs available\n");
        goto fail;
    }
    EGLConfig *configs = malloc(sizeof(EGLConfig) * num_configs);
    if (!configs) {
        goto fail;
    }
    if (!eglGetConfigs(e->display, configs, num_configs, &num_configs)) {
        free(configs);
        goto fail;
    }

    ConfigScore chosen_score = {0};
    int requested_interval = e->no_vsync ? 0 : 1;
    EGLConfig chosen_config = egl_select_best_config(e->display, configs, num_configs,
                                                     overlay_visual_id, requested_interval,
                                                     &chosen_score);
    free(configs);

    if (!chosen_config) {
        fprintf(stderr, "slopcom: No matching EGLConfig found for overlay visual 0x%lx with GLES2 and window surface\n",
                (unsigned long)overlay_visual_id);
        goto fail;
    }
    e->config = chosen_config;

    fprintf(stderr, "slopcom: Chosen EGL config: visual ID 0x%lx, depth %d, stencil %d, samples %d\n",
            (unsigned long)overlay_visual_id, chosen_score.depth, chosen_score.stencil, chosen_score.samples);
    if (chosen_score.depth == 0 && chosen_score.stencil == 0 && chosen_score.samples == 0) {
        fprintf(stderr, "slopcom: Config has no unnecessary depth, stencil, or multisample buffers\n");
    } else {
        fprintf(stderr, "slopcom: Retained fallback config with depth=%d, stencil=%d, samples=%d\n",
                chosen_score.depth, chosen_score.stencil, chosen_score.samples);
    }

    /* Create GLES2 Context */
    const EGLint ctx_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    e->context = eglCreateContext(e->display, e->config, EGL_NO_CONTEXT, ctx_attribs);
    if (e->context == EGL_NO_CONTEXT) {
        fprintf(stderr, "slopcom: eglCreateContext failed: 0x%x\n", eglGetError());
        goto fail;
    }

    /* Create Surface */
    if (e->is_platform_ext && create_platform_window_surface) {
        e->surface = create_platform_window_surface(e->display, e->config, (void *)&overlay, NULL);
    } else {
        e->surface = eglCreateWindowSurface(e->display, e->config, (EGLNativeWindowType)overlay, NULL);
    }
    if (e->surface == EGL_NO_SURFACE) {
        fprintf(stderr, "slopcom: eglCreateWindowSurface failed: 0x%x\n", eglGetError());
        goto fail;
    }

    /* Make Current */
    if (!eglMakeCurrent(e->display, e->surface, e->surface, e->context)) {
        fprintf(stderr, "slopcom: eglMakeCurrent failed: 0x%x\n", eglGetError());
        goto fail;
    }
    e->has_current_context = true;

    /* Query and check EGL extensions */
    const char *disp_exts = eglQueryString(e->display, EGL_EXTENSIONS);
    if (!disp_exts) {
        fprintf(stderr, "slopcom: Failed to query EGL display extensions\n");
        goto fail;
    }
    bool has_image_pixmap = strstr(disp_exts, "EGL_KHR_image_pixmap") != NULL;
    bool has_image_base = (strstr(disp_exts, "EGL_KHR_image_base") != NULL ||
                           strstr(disp_exts, "EGL_KHR_image") != NULL);
    if (!has_image_pixmap || !has_image_base) {
        fprintf(stderr, "slopcom: Required EGLImage pixmap extensions missing (image_pixmap=%d, image_base=%d)\n",
                has_image_pixmap, has_image_base);
        goto fail;
    }

    /* Query GLES extensions */
    const char *gl_exts = (const char *)glGetString(GL_EXTENSIONS);
    if (!gl_exts || strstr(gl_exts, "GL_OES_EGL_image") == NULL) {
        fprintf(stderr, "slopcom: Required GLES extension GL_OES_EGL_image missing\n");
        goto fail;
    }

    /* Load function pointers */
    e->create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    e->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    e->bind_image = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!e->create_image || !e->destroy_image || !e->bind_image) {
        fprintf(stderr, "slopcom: Failed to load required EGL/GL image function pointers\n");
        goto fail;
    }

    /* Check buffer age, partial update, and swap-with-damage extensions */
    e->has_buffer_age = (strstr(disp_exts, "EGL_EXT_buffer_age") != NULL ||
                         strstr(disp_exts, "EGL_KHR_partial_update") != NULL);
    e->has_partial_update = (strstr(disp_exts, "EGL_KHR_partial_update") != NULL);
    e->has_swap_buffers_with_damage = (strstr(disp_exts, "EGL_EXT_swap_buffers_with_damage") != NULL ||
                                       strstr(disp_exts, "EGL_KHR_swap_buffers_with_damage") != NULL);

    if (e->has_partial_update) {
        e->set_damage_region = (PFNEGLSETDAMAGEREGIONKHRPROC)eglGetProcAddress("eglSetDamageRegionKHR");
        if (!e->set_damage_region) e->has_partial_update = false;
    }
    if (e->has_swap_buffers_with_damage) {
        e->swap_buffers_with_damage = (PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC)eglGetProcAddress("eglSwapBuffersWithDamageEXT");
        if (!e->swap_buffers_with_damage) {
            e->swap_buffers_with_damage = (PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC)eglGetProcAddress("eglSwapBuffersWithDamageKHR");
        }
        if (!e->swap_buffers_with_damage) e->has_swap_buffers_with_damage = false;
    }

    /* Check swap interval range */
    EGLint min_interval = 0, max_interval = 0;
    eglGetConfigAttrib(e->display, e->config, EGL_MIN_SWAP_INTERVAL, &min_interval);
    eglGetConfigAttrib(e->display, e->config, EGL_MAX_SWAP_INTERVAL, &max_interval);
    if (min_interval > requested_interval || max_interval < requested_interval) {
        fprintf(stderr, "slopcom: EGLConfig swap interval range [%d, %d] does not include %d\n",
                min_interval, max_interval, requested_interval);
        goto fail;
    }

    if (!eglSwapInterval(e->display, requested_interval)) {
        fprintf(stderr, "slopcom: eglSwapInterval(%d) failed: 0x%x\n", requested_interval, eglGetError());
        goto fail;
    }

    /* Log diagnostics as required by PLAN.md */
    printf("slopcom: EGL initialized (%d.%d)\n", major, minor);
    printf("slopcom: EGL vendor: %s\n", eglQueryString(e->display, EGL_VENDOR));
    printf("slopcom: GLES renderer: %s\n", (const char *)glGetString(GL_RENDERER));
    printf("slopcom: GLES version: %s\n", (const char *)glGetString(GL_VERSION));
    printf("slopcom: Chosen overlay visual ID: 0x%lx\n", (unsigned long)overlay_visual_id);
    printf("slopcom: Swap interval range: [%d, %d], requested interval %d (success)\n",
           min_interval, max_interval, requested_interval);
    printf("slopcom: EGL buffer age: %s\n", e->has_buffer_age ? "supported" : "unsupported");
    printf("slopcom: EGL partial update: %s\n", e->has_partial_update ? "supported" : "unsupported");
    printf("slopcom: EGL swap with damage: %s\n", e->has_swap_buffers_with_damage ? "supported" : "unsupported");

    glViewport(0, 0, width, height);
    return true;

fail:
    egl_destroy(e, xdisplay);
    return false;
}

bool egl_import_pixmap(EGLState *e, Pixmap pixmap, ImportedPixmap *out) {
    out->image = EGL_NO_IMAGE_KHR;
    out->texture = 0;

    const EGLint attrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    out->image = e->create_image(e->display, EGL_NO_CONTEXT,
        EGL_NATIVE_PIXMAP_KHR, (EGLClientBuffer)(uintptr_t)pixmap, attrs);
    if (out->image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "slopcom: eglCreateImageKHR failed for pixmap 0x%lx (error 0x%x)\n",
                (unsigned long)pixmap, eglGetError());
        goto fail;
    }
    if (e->stats) e->stats->live_images++;

    glGenTextures(1, &out->texture);
    if (out->texture == 0) {
        fprintf(stderr, "slopcom: glGenTextures failed for pixmap 0x%lx (glError 0x%x)\n",
                (unsigned long)pixmap, glGetError());
        goto fail;
    }
    if (e->stats) e->stats->live_textures++;
    glBindTexture(GL_TEXTURE_2D, out->texture);
    e->bind_image(GL_TEXTURE_2D, (GLeglImageOES)out->image);
    GLenum gl_err = glGetError();
    if (gl_err != GL_NO_ERROR) {
        fprintf(stderr, "slopcom: glEGLImageTargetTexture2DOES failed for pixmap 0x%lx (glError 0x%x)\n",
                (unsigned long)pixmap, gl_err);
        goto fail;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;

fail:
    egl_release_import(e, out);
    return false;
}

void egl_release_import(EGLState *e, ImportedPixmap *imp) {
    if (imp->texture != 0) {
        glDeleteTextures(1, &imp->texture);
        imp->texture = 0;
        if (e && e->stats && e->stats->live_textures > 0) {
            e->stats->live_textures--;
        }
    }
    if (imp->image != EGL_NO_IMAGE_KHR) {
        e->destroy_image(e->display, imp->image);
        imp->image = EGL_NO_IMAGE_KHR;
        if (e && e->stats && e->stats->live_images > 0) {
            e->stats->live_images--;
        }
    }
}

bool egl_present(EGLState *e) {
    bool ok = false;
    if (e->has_swap_buffers_with_damage && !e->full_redraw &&
        e->presentation_damage_count > 0) {
        ok = e->swap_buffers_with_damage(e->display, e->surface,
                                         e->presentation_damage,
                                         e->presentation_damage_count);
        e->presentation_damage_count = 0;
    } else {
        ok = eglSwapBuffers(e->display, e->surface);
        e->presentation_damage_count = 0;
    }
    if (!ok) {
        EGLint err = eglGetError();
        fprintf(stderr, "slopcom: presentation failed: 0x%x\n", err);
        return false;
    }
    return true;
}

void egl_destroy(EGLState *e, Display *xdisplay) {
    (void)xdisplay;
    e->presentation_damage_count = 0;
    if (e->display != EGL_NO_DISPLAY) {
        if (e->has_current_context) {
            eglMakeCurrent(e->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            e->has_current_context = false;
        }
        if (e->surface != EGL_NO_SURFACE) {
            eglDestroySurface(e->display, e->surface);
            e->surface = EGL_NO_SURFACE;
        }
        if (e->context != EGL_NO_CONTEXT) {
            eglDestroyContext(e->display, e->context);
            e->context = EGL_NO_CONTEXT;
        }
        if (e->is_initialized) {
            eglTerminate(e->display);
            e->is_initialized = false;
        }
        e->display = EGL_NO_DISPLAY;
    }
}
