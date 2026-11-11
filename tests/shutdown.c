#include "../src/x11.h"
#undef ConnectionNumber
#define ConnectionNumber(dpy) (3)
#define main compositor_main
#include "../src/main.c"
#undef main

static bool context_current, imports_live;
static bool invalid_cleanup;
static bool fail_x11, fail_egl, fail_render, fail_reconcile, mock_x_lost;
static int x11_init_calls, egl_init_calls, render_init_calls, reconcile_calls;
static int x11_destroy_calls, egl_destroy_calls, render_destroy_calls;
static int x11_release_calls;
static int poll_calls, present_calls;
static bool pending_work_case, recovery_due, recovery_queued;
static bool expose_loop_case, expose_sent;
static X11State *mock_x11_state;
static bool captured_unredir_fullscreen;

bool x11_init(X11State *x, const char *name) {
    (void)name;
    x11_init_calls++;
    if (fail_x11) return false;
    x->display = (Display *)(uintptr_t)1;
    x->overlay = 9;
    mock_x11_state = x;
    captured_unredir_fullscreen = x->unredir_fullscreen;
    return true;
}

bool egl_init(EGLState *e, Display *dpy, Window overlay, VisualID visual, int w, int h) {
    (void)e; (void)dpy; (void)overlay; (void)visual; (void)w; (void)h;
    egl_init_calls++;
    if (fail_egl) return false;
    context_current = true;
    return true;
}

bool render_init(RenderState *r, int w, int h) {
    (void)r; (void)w; (void)h;
    render_init_calls++;
    if (fail_render) return false;
    return true;
}

bool x11_reconcile(X11State *x, EGLState *e) {
    (void)e;
    reconcile_calls++;
    if (fail_reconcile) return false;
    imports_live = true;
    if (pending_work_case && reconcile_calls > 1) g_stop_requested = 1;
    (void)x;
    return true;
}

int XPending(Display *dpy) {
    (void)dpy;
    if (pending_work_case) {
        if (recovery_due && !recovery_queued) {
            recovery_queued = true;
            mock_x11_state->pending_reconcile = true;
        }
        return 0;
    }
    if (expose_loop_case && present_calls > 0 && !expose_sent) return 1;
    return expose_loop_case ? 0 : 1;
}
int XNextEvent(Display *dpy, XEvent *ev) {
    (void)dpy;
    memset(ev, 0, sizeof(*ev));
    if (expose_loop_case && present_calls > 0 && !expose_sent) {
        ev->type = Expose;
        ev->xexpose.window = mock_x11_state->overlay;
        expose_sent = true;
    }
    return 0;
}
int XFlush(Display *dpy) { (void)dpy; return 0; }
int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    (void)fds; (void)nfds; (void)timeout;
    poll_calls++;
    return 0;
}
Status XGetWindowAttributes(Display *dpy, Window w, XWindowAttributes *a) {
    (void)dpy; (void)w; memset(a, 0, sizeof(*a)); a->width = 200; a->height = 200; return True;
}
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { (void)x; (void)y; (void)w; (void)h; }
bool x11_damage_rect(X11State *x, int rx, int ry, int rw, int rh) {
    x->client_damage.count = 1;
    x->client_damage.rects[0] = (DamageRect){ rx, ry, rw, rh };
    return true;
}

void x11_handle_event(X11State *x, XEvent *ev, bool *dirty, bool *stop) {
    if (expose_loop_case && ev->type == Expose && ev->xexpose.window == x->overlay) {
        x->needs_full_repaint = true;
        *dirty = true;
        return;
    }
    (void)x; (void)ev; (void)dirty;
    *stop = true;
}

void x11_finish_damage_batch(X11State *x) { (void)x; }
void x11_invalidate_full_scene(X11State *x) {
    if (!x) return;
    x->history_count = x->history_head = 0;
    for (int i = 0; i < DAMAGE_HISTORY_CAP; i++) damage_list_clear(&x->client_history[i]);
    damage_list_clear(&x->client_damage);
    x->needs_full_repaint = true;
    damage_list_add(&x->client_damage, 0, 0, x->root_width, x->root_height);
}
bool x11_reconcile_and_advance_fullscreen(X11State *x, EGLState *e, bool *dirty) {
    (void)x; (void)e; (void)dirty;
    return true;
}
bool x11_finish_fullscreen_presentation(X11State *x, EGLState *e, bool *dirty) {
    (void)x; (void)e; (void)dirty;
    return true;
}
bool render_scene(RenderState *r, X11State *x, int w, int h) {
    (void)r; (void)w; (void)h;
    if (!x11_repaint_pending(x)) return false;
    x->needs_full_repaint = false;
    x->client_damage.count = 0;
    return true;
}
bool egl_present(EGLState *e) {
    (void)e;
    present_calls++;
    if (pending_work_case && present_calls == 1) recovery_due = true;
    if (expose_loop_case && present_calls >= 2) g_stop_requested = 1;
    return true;
}
void background_refresh(BackgroundState *bg, EGLState *egl) {
    (void)egl;
    if (bg) bg->refresh_required = false;
}

void render_destroy(RenderState *r) {
    (void)r;
    render_destroy_calls++;
    if (!context_current) invalid_cleanup = true;
}

void x11_release_windows(X11State *x, EGLState *e) {
    (void)x; (void)e;
    x11_release_calls++;
    if (imports_live && !context_current) invalid_cleanup = true;
    imports_live = false;
}

void egl_destroy(EGLState *e, Display *dpy) {
    (void)e; (void)dpy;
    egl_destroy_calls++;
    if (imports_live) invalid_cleanup = true;
    context_current = false;
}

void x11_destroy(X11State *x, EGLState *e) {
    x11_destroy_calls++;
    x11_release_windows(x, e);
    x->display = NULL;
}

static void reset_test_state(void) {
    context_current = imports_live = invalid_cleanup = false;
    fail_x11 = fail_egl = fail_render = fail_reconcile = mock_x_lost = false;
    x11_init_calls = egl_init_calls = render_init_calls = reconcile_calls = 0;
    x11_destroy_calls = egl_destroy_calls = render_destroy_calls = x11_release_calls = 0;
    poll_calls = present_calls = 0;
    pending_work_case = recovery_due = recovery_queued = false;
    expose_loop_case = expose_sent = false;
    mock_x11_state = NULL;
    captured_unredir_fullscreen = false;
    g_stop_requested = g_stats_requested = 0;
}

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        return 1; \
    } \
} while (0)

int main(void) {
    /* 1. CLI help flag returns exit 0 */
    {
        reset_test_state();
        char *argv[] = { "slopcom", "--help", NULL };
        int rc = compositor_main(2, argv);
        TEST_ASSERT(rc == 0, "CLI --help exits with 0");
    }

    /* 1b. CLI --full-redraw option accepted and runs healthy */
    {
        reset_test_state();
        char *argv[] = { "slopcom", "--full-redraw", NULL };
        int rc = compositor_main(2, argv);
        TEST_ASSERT(rc == 0, "CLI --full-redraw exits with 0");
        TEST_ASSERT(!invalid_cleanup, "no invalid cleanup with --full-redraw");
    }

    /* 1c. CLI --no-vsync option accepted and runs healthy */
    {
        reset_test_state();
        char *argv[] = { "slopcom", "--no-vsync", NULL };
        int rc = compositor_main(2, argv);
        TEST_ASSERT(rc == 0, "CLI --no-vsync exits with 0");
        TEST_ASSERT(!invalid_cleanup, "no invalid cleanup with --no-vsync");
    }

    /* 1d. CLI --unredir-fullscreen option accepted and runs healthy */
    {
        reset_test_state();
        char *argv[] = { "slopcom", "--unredir-fullscreen", NULL };
        int rc = compositor_main(2, argv);
        TEST_ASSERT(rc == 0, "CLI --unredir-fullscreen exits with 0");
        TEST_ASSERT(!invalid_cleanup, "no invalid cleanup with --unredir-fullscreen");
        TEST_ASSERT(captured_unredir_fullscreen,
                    "unredir_fullscreen enabled in X11State");
    }

    /* 1e. CLI --full-redraw precedence over --unredir-fullscreen (both orders) */
    {
        reset_test_state();
        char *argv1[] = { "slopcom", "--unredir-fullscreen", "--full-redraw", NULL };
        int rc1 = compositor_main(3, argv1);
        TEST_ASSERT(rc1 == 0, "CLI --unredir-fullscreen then --full-redraw exits 0");
        TEST_ASSERT(!captured_unredir_fullscreen,
                    "--full-redraw disables unredir_fullscreen when passed second");

        reset_test_state();
        char *argv2[] = { "slopcom", "--full-redraw", "--unredir-fullscreen", NULL };
        int rc2 = compositor_main(3, argv2);
        TEST_ASSERT(rc2 == 0, "CLI --full-redraw then --unredir-fullscreen exits 0");
        TEST_ASSERT(!captured_unredir_fullscreen,
                    "--full-redraw disables unredir_fullscreen when passed first");
    }

    /* 1f. has_pending_internal_work predicate suppresses paint-only flags when bypassed */
    {
        App app = {0};
        app.x11.fullscreen_recheck = true;
        TEST_ASSERT(!has_pending_internal_work(&app),
                    "default-off fullscreen policy does not schedule composited work");
        app.x11.unredir_fullscreen = true;
        TEST_ASSERT(has_pending_internal_work(&app),
                    "enabled fullscreen policy schedules its initial evaluation");

        app.x11.fullscreen_mode = FULLSCREEN_BYPASSED;
        app.x11.fullscreen_recheck = false;
        app.x11.needs_full_repaint = true;
        app.x11.scene_dirty = true;
        app.x11.client_damage.count = 2;
        TEST_ASSERT(!has_pending_internal_work(&app),
                    "paint-only work does not trigger internal work when bypassed");

        app.x11.fullscreen_recheck = true;
        TEST_ASSERT(has_pending_internal_work(&app),
                    "fullscreen_recheck triggers internal work when bypassed");
        app.x11.fullscreen_recheck = false;

        app.x11.pending_reconcile = true;
        TEST_ASSERT(has_pending_internal_work(&app),
                    "pending_reconcile triggers internal work when bypassed");
        app.x11.pending_reconcile = false;

        app.x11.root_size_changed = true;
        TEST_ASSERT(has_pending_internal_work(&app),
                    "root_size_changed triggers internal work when bypassed");
        app.x11.root_size_changed = false;

        app.x11.background.refresh_required = true;
        TEST_ASSERT(has_pending_internal_work(&app),
                    "background refresh triggers internal work when bypassed");
    }

    /* 2. CLI unknown option returns exit 2 */
    {
        reset_test_state();
        char *argv[] = { "slopcom", "--bogus-flag", NULL };
        int rc = compositor_main(2, argv);
        TEST_ASSERT(rc == 2, "CLI unknown option exits with 2");
    }

    /* 3. CLI extra positional arguments returns exit 2 */
    {
        reset_test_state();
        char *argv[] = { "slopcom", ":0", ":1", NULL };
        int rc = compositor_main(3, argv);
        TEST_ASSERT(rc == 2, "CLI multiple positional arguments exit with 2");
    }

    /* 4. Healthy startup and shutdown */
    {
        reset_test_state();
        char *argv[] = { "slopcom", NULL };
        int rc = compositor_main(1, argv);
        TEST_ASSERT(rc == 0, "healthy run exits with 0");
        TEST_ASSERT(!invalid_cleanup, "no invalid cleanup order");
        TEST_ASSERT(!imports_live, "imports released");
        TEST_ASSERT(!context_current, "context detached");
        TEST_ASSERT(egl_destroy_calls == 1 && x11_destroy_calls == 1, "destroy called once");
    }

    /* An error dispatched by XPending schedules reconciliation without an X event. */
    {
        reset_test_state();
        pending_work_case = true;
        char *argv[] = { "slopcom", NULL };
        int rc = compositor_main(1, argv);
        TEST_ASSERT(rc == 0, "error-only recovery run exits cleanly");
        TEST_ASSERT(reconcile_calls == 2, "pending recovery is reconciled after the first frame");
        TEST_ASSERT(poll_calls == 0, "internal recovery work bypasses blocking poll");
        TEST_ASSERT(present_calls == 1, "recovery without repaint work does not add a frame");
    }

    /* Overlay exposure after startup must schedule a real frame through main's gate. */
    {
        reset_test_state();
        expose_loop_case = true;
        char *argv[] = { "slopcom", NULL };
        int rc = compositor_main(1, argv);
        TEST_ASSERT(rc == 0, "expose repair run exits cleanly");
        TEST_ASSERT(present_calls == 2, "overlay exposure schedules a second presentation");
        TEST_ASSERT(poll_calls == 0, "expose repair is presented before blocking");
    }

    /* Max-sized counters and timing values must serialize completely and safely. */
    {
        Stats boundary;
        stats_init(&boundary);
#define MAX_FIELD(field) boundary.field = UINT64_MAX
        MAX_FIELD(x_events); MAX_FIELD(damage_events); MAX_FIELD(render_attempts);
        MAX_FIELD(successful_swaps); MAX_FIELD(full_repaints); MAX_FIELD(partial_repaints);
        MAX_FIELD(skipped_presentations); MAX_FIELD(skipped_occluded_damage); MAX_FIELD(poll_wakeups);
        MAX_FIELD(imports); MAX_FIELD(reimports); MAX_FIELD(x_round_trips);
        MAX_FIELD(live_pixmaps); MAX_FIELD(live_images); MAX_FIELD(live_textures);
        MAX_FIELD(live_damage); MAX_FIELD(live_regions);
#undef MAX_FIELD
        boundary.event_reconcile_ns.total_samples = UINT64_MAX;
        boundary.event_reconcile_ns.count = 1;
        boundary.event_reconcile_ns.samples[0] = UINT64_MAX;
        boundary.draw_ns.total_samples = UINT64_MAX;
        boundary.draw_ns.count = 1;
        boundary.draw_ns.samples[0] = UINT64_MAX;
        boundary.swap_ns.total_samples = UINT64_MAX;
        boundary.swap_ns.count = 1;
        boundary.swap_ns.samples[0] = UINT64_MAX;

        int fds[2];
        TEST_ASSERT(pipe(fds) == 0, "create stats capture pipe");
        stats_dump(&boundary, "boundary\"line\n", true, fds[1]);
        close(fds[1]);
        char json[4096];
        ssize_t json_len = read(fds[0], json, sizeof(json) - 1);
        close(fds[0]);
        TEST_ASSERT(json_len > 0 && (size_t)json_len < sizeof(json) - 1,
                    "detailed stats output fits capture capacity");
        json[json_len] = '\0';
        TEST_ASSERT(json[json_len - 1] == '\n' && strstr(json, "\"kind\":\"boundary\\\"line\\n\"") != NULL,
                    "stats kind is escaped and output ends with a newline");
        TEST_ASSERT(strstr(json, "18446744073709551615") != NULL &&
                    strstr(json, "\"p99\":18446744073709551615}}}\n") != NULL,
                    "detailed JSON retains maximal counters and timing values");

        TEST_ASSERT(pipe(fds) == 0, "create human stats capture pipe");
        stats_dump_human(&boundary, -1, fds[1]);
        close(fds[1]);
        char human[512];
        ssize_t human_len = read(fds[0], human, sizeof(human) - 1);
        close(fds[0]);
        TEST_ASSERT(human_len > 0 && (size_t)human_len < sizeof(human) - 1,
                    "human stats output fits bounded buffer");
        human[human_len] = '\0';
        TEST_ASSERT(human[human_len - 1] == '\n' &&
                    strstr(human, "live_textures=18446744073709551615") != NULL,
                    "human stats output is complete at counter limits");
    }

    /* 5. Injected failure in x11_init */
    {
        reset_test_state();
        fail_x11 = true;
        char *argv[] = { "slopcom", NULL };
        int rc = compositor_main(1, argv);
        TEST_ASSERT(rc == 1, "x11_init failure exits with 1");
        TEST_ASSERT(egl_init_calls == 0, "egl_init not called on x11_init failure");
        TEST_ASSERT(render_init_calls == 0, "render_init not called on x11_init failure");
        TEST_ASSERT(!invalid_cleanup, "clean rollback on x11_init failure");
    }

    /* 6. Injected failure in egl_init */
    {
        reset_test_state();
        fail_egl = true;
        char *argv[] = { "slopcom", NULL };
        int rc = compositor_main(1, argv);
        TEST_ASSERT(rc == 1, "egl_init failure exits with 1");
        TEST_ASSERT(render_init_calls == 0, "render_init not called on egl_init failure");
        TEST_ASSERT(x11_destroy_calls == 1, "x11_destroy called on egl_init failure");
        TEST_ASSERT(!invalid_cleanup, "clean rollback on egl_init failure");
    }

    /* 7. Injected failure in render_init */
    {
        reset_test_state();
        fail_render = true;
        char *argv[] = { "slopcom", NULL };
        int rc = compositor_main(1, argv);
        TEST_ASSERT(rc == 1, "render_init failure exits with 1");
        TEST_ASSERT(reconcile_calls == 0, "reconcile not called on render_init failure");
        TEST_ASSERT(egl_destroy_calls == 1 && x11_destroy_calls == 1, "cleanup called on render failure");
        TEST_ASSERT(!invalid_cleanup, "clean rollback on render_init failure");
    }

    /* 8. Injected failure in x11_reconcile */
    {
        reset_test_state();
        fail_reconcile = true;
        char *argv[] = { "slopcom", NULL };
        int rc = compositor_main(1, argv);
        TEST_ASSERT(rc == 1, "x11_reconcile failure exits with 1");
        TEST_ASSERT(egl_destroy_calls == 1 && x11_destroy_calls == 1, "cleanup called on reconcile failure");
        TEST_ASSERT(!invalid_cleanup, "clean rollback on reconcile failure");
    }

    {
        App app = {0};
        app.x11.root_width = 100; app.x11.root_height = 100;
        app.x11.root_size_changed = true;
        app.x11.history_count = 3; app.x11.history_head = 3;
        bool dirty = false;
        TEST_ASSERT(reconcile_scene(&app, &dirty), "root resize reconciles");
        TEST_ASSERT(dirty && app.x11.history_count == 0 && app.x11.history_head == 0,
                    "root resize discards old buffer history");
        TEST_ASSERT(app.x11.client_damage.count == 1 &&
                    app.x11.client_damage.rects[0].width == 200 &&
                    app.x11.client_damage.rects[0].height == 200,
                    "root resize invalidates the entire confirmed new size");
    }
    puts("PASS: shutdown, startup failures, and root resize");
    return 0;
}
