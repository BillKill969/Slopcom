#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>

#include "x11.h"
#include "egl.h"
#include "render.h"
#include "stats.h"

typedef struct App {
    X11State x11;
    EGLState egl;
    RenderState render;
    Stats stats;
    int sig_pipe[2];
    bool verbose;
    bool detailed_stats;
    bool full_redraw;
    bool no_vsync;
} App;

typedef enum {
    WAIT_READY,
    WAIT_INTERRUPTED,
    WAIT_X_LOST,
    WAIT_ERROR
} WaitResult;

static volatile sig_atomic_t g_stop_requested = 0;
static volatile sig_atomic_t g_stats_requested = 0;
static int s_sig_pipe_write_fd = -1;

static void signal_handler(int sig) {
    int saved_errno = errno;
    if (sig == SIGINT || sig == SIGTERM) {
        g_stop_requested = 1;
    } else if (sig == SIGUSR1) {
        g_stats_requested = 1;
    }
    if (s_sig_pipe_write_fd >= 0) {
        char b = 1;
        (void)write(s_sig_pipe_write_fd, &b, 1);
    }
    errno = saved_errno;
}

static bool configure_pipe_fd(int fd) {
    int status = fcntl(fd, F_GETFL);
    if (status == -1 || fcntl(fd, F_SETFL, status | O_NONBLOCK) == -1)
        return false;
    int descriptor = fcntl(fd, F_GETFD);
    return descriptor != -1 &&
        fcntl(fd, F_SETFD, descriptor | FD_CLOEXEC) != -1;
}

static void service_signals(App *app, bool *stop_requested) {
    if (app->sig_pipe[0] >= 0) {
        char buf[64];
        while (read(app->sig_pipe[0], buf, sizeof(buf)) > 0) {}
    }
    if (g_stop_requested) {
        *stop_requested = true;
    }
    if (g_stats_requested) {
        g_stats_requested = 0;
        stats_dump(&app->stats, "snapshot", app->detailed_stats, STDOUT_FILENO);
    }
}

static WaitResult wait_for_x_or_signal(App *app) {
    XFlush(app->x11.display);

    struct pollfd fds[2];
    fds[0].fd = ConnectionNumber(app->x11.display);
    fds[0].events = POLLIN;
    fds[0].revents = 0;

    fds[1].fd = app->sig_pipe[0];
    fds[1].events = POLLIN;
    fds[1].revents = 0;

    int ret = poll(fds, 2, -1);
    if (ret < 0) {
        if (errno == EINTR) {
            return WAIT_INTERRUPTED;
        }
        fprintf(stderr, "slopcom: poll error: %s\n", strerror(errno));
        return WAIT_ERROR;
    }

    if (ret > 0) {
        app->stats.poll_wakeups++;
    }

    if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        fprintf(stderr, "slopcom: Fatal X connection poll error (revents 0x%x)\n", fds[0].revents);
        return WAIT_X_LOST;
    }

    return WAIT_READY;
}

static void process_event_batch(App *app, bool *dirty, bool *stop_requested) {
    int count = 0;
    while (XPending(app->x11.display) > 0 && count < X11_EVENT_BATCH_CAP) {
        XEvent ev;
        XNextEvent(app->x11.display, &ev);
        x11_handle_event(&app->x11, &ev, dirty, stop_requested);
        count++;
        if (*stop_requested) break;
    }
}

static bool has_pending_internal_work(const App *app) {
    if (app->x11.fullscreen_mode == FULLSCREEN_BYPASSED) {
        return app->x11.pending_reconcile || app->x11.root_size_changed ||
            app->x11.background.refresh_required || app->x11.fullscreen_recheck ||
            app->x11.damage_request_count > 0;
    }
    return app->x11.pending_reconcile || app->x11.root_size_changed ||
        app->x11.background.refresh_required || app->x11.scene_dirty ||
        (app->x11.unredir_fullscreen && app->x11.fullscreen_recheck) ||
        app->x11.damage_request_count > 0 ||
        x11_repaint_pending(&app->x11);
}

static bool reconcile_scene(App *app, bool *dirty) {
    if (app->x11.fullscreen_mode == FULLSCREEN_BYPASSED) {
        if (app->x11.pending_reconcile) {
            app->x11.pending_reconcile = false;
            if (!x11_reconcile(&app->x11, &app->egl)) {
                return false;
            }
        }
        return true;
    }

    if (app->x11.root_size_changed) {
        app->x11.root_size_changed = false;
        XWindowAttributes wa;
        if (XGetWindowAttributes(app->x11.display, app->x11.root, &wa)) {
            app->x11.root_width = wa.width;
            app->x11.root_height = wa.height;
            app->egl.presentation_damage_count = 0;
            glViewport(0, 0, wa.width, wa.height);
            app->x11.background.refresh_required = true;
            x11_invalidate_full_scene(&app->x11);
            *dirty = true;
        }
    }
    if (app->x11.background.refresh_required) {
        background_refresh(&app->x11.background, &app->egl);
        app->egl.presentation_damage_count = 0;
        x11_invalidate_full_scene(&app->x11);
        *dirty = true;
    }
    if (app->x11.pending_reconcile) {
        app->x11.pending_reconcile = false;
        if (!x11_reconcile(&app->x11, &app->egl)) {
            return false;
        }
        if (app->x11.scene_dirty) {
            app->x11.scene_dirty = false;
            *dirty = true;
        }
    }
    return true;
}

int main(int argc, char **argv) {
    const char *display_name = NULL;
    bool verbose = false;
    bool stats = false;
    bool full_redraw = false;
    bool no_vsync = false;
    bool unredir_fullscreen = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: slopcom [OPTIONS] [DISPLAY]\n\n"
                   "Options:\n"
                   "  -h, --help                Show this help and exit\n"
                   "  --verbose                 Enable verbose output\n"
                   "  --stats                   Enable detailed measurement statistics\n"
                   "  --full-redraw             Force full scene repaint on every dirty frame (golden reference)\n"
                   "  --no-vsync                Disable VSync presentation synchronization (diagnostic)\n"
                   "  --unredir-fullscreen      Bypass compositing when an eligible fullscreen application covers the root\n");
            return 0;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--stats") == 0) {
            stats = true;
        } else if (strcmp(argv[i], "--full-redraw") == 0) {
            full_redraw = true;
        } else if (strcmp(argv[i], "--no-vsync") == 0) {
            no_vsync = true;
        } else if (strcmp(argv[i], "--unredir-fullscreen") == 0) {
            unredir_fullscreen = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "slopcom: Unknown option '%s'\n", argv[i]);
            return 2;
        } else {
            if (display_name != NULL) {
                fprintf(stderr, "slopcom: Unexpected argument '%s' (DISPLAY already set to '%s')\n",
                        argv[i], display_name);
                return 2;
            }
            display_name = argv[i];
        }
    }

    if (full_redraw) {
        unredir_fullscreen = false;
    }

    App app;
    memset(&app, 0, sizeof(app));
    app.verbose = verbose;
    app.detailed_stats = stats;
    app.full_redraw = full_redraw;
    app.no_vsync = no_vsync;
    app.x11.full_redraw = full_redraw;
    app.x11.unredir_fullscreen = unredir_fullscreen;
    app.egl.full_redraw = full_redraw;
    app.egl.no_vsync = no_vsync;
    app.render.full_redraw = full_redraw;
    stats_init(&app.stats);
    app.x11.stats = &app.stats;
    app.egl.stats = &app.stats;
    app.sig_pipe[0] = -1;
    app.sig_pipe[1] = -1;

    int exit_status = 0;

    /* Initialize self-pipe for signal wakeup */
    if (pipe(app.sig_pipe) != 0) {
        perror("slopcom: pipe");
        return 1;
    }
    if (!configure_pipe_fd(app.sig_pipe[0]) || !configure_pipe_fd(app.sig_pipe[1])) {
        fprintf(stderr, "slopcom: Failed to configure signal pipe descriptors\n");
        close(app.sig_pipe[0]);
        close(app.sig_pipe[1]);
        return 1;
    }
    s_sig_pipe_write_fd = app.sig_pipe[1];

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) != 0 ||
        sigaction(SIGTERM, &sa, NULL) != 0 ||
        sigaction(SIGUSR1, &sa, NULL) != 0) {
        fprintf(stderr, "slopcom: Failed to install signal handlers\n");
        close(app.sig_pipe[0]);
        close(app.sig_pipe[1]);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    bool x11_initialized = false;
    bool egl_initialized = false;
    bool render_initialized = false;

    /* Step 1: Initialize X11 and compositing manager */
    if (!x11_init(&app.x11, display_name)) {
        exit_status = 1;
        goto cleanup;
    }
    x11_initialized = true;

    /* Step 2: Initialize EGL and GLES2 */
    if (!egl_init(&app.egl, app.x11.display, app.x11.overlay, app.x11.overlay_visual_id,
                 app.x11.root_width, app.x11.root_height)) {
        exit_status = 1;
        goto cleanup;
    }
    egl_initialized = true;

    /* Step 3: Initialize renderer */
    if (!render_init(&app.render, app.x11.root_width, app.x11.root_height)) {
        exit_status = 1;
        goto cleanup;
    }
    render_initialized = true;
    app.render.egl = &app.egl;

    /* Step 4: Reconcile initial windows */
    if (!x11_reconcile(&app.x11, &app.egl)) {
        exit_status = 1;
        goto cleanup;
    }

    app.x11.needs_full_repaint = true;
    bool dirty = true;
    bool stop_requested = false;

    while (!stop_requested) {
        service_signals(&app, &stop_requested);
        if (stop_requested) break;

        /* XPending may read the socket and dispatch an asynchronous X error,
         * which can schedule internal recovery without queuing an event. */
        int pending_events = XPending(app.x11.display);
        if (!dirty && pending_events == 0 && !has_pending_internal_work(&app)) {
            WaitResult wr = wait_for_x_or_signal(&app);
            if (wr == WAIT_X_LOST) {
                if (app.sig_pipe[0] >= 0) close(app.sig_pipe[0]);
                if (app.sig_pipe[1] >= 0) close(app.sig_pipe[1]);
                return 1;
            } else if (wr == WAIT_ERROR) {
                exit_status = 1;
                goto cleanup;
            }
        }

        uint64_t t_er0 = 0, t_er1 = 0;
        if (app.detailed_stats) stats_monotonic_ns(&t_er0);

        bool had_events = (XPending(app.x11.display) > 0);
        process_event_batch(&app, &dirty, &stop_requested);
        x11_finish_damage_batch(&app.x11);
        if (stop_requested) break;

        if (!reconcile_scene(&app, &dirty)) {
            exit_status = 1;
            goto cleanup;
        }

        if (!x11_reconcile_and_advance_fullscreen(&app.x11, &app.egl, &dirty)) {
            exit_status = 1;
            goto cleanup;
        }

        if (app.x11.fullscreen_mode == FULLSCREEN_BYPASSED) {
            dirty = false;
            if (had_events) {
                app.stats.skipped_presentations++;
            }
            continue;
        }

        if (app.detailed_stats && t_er0 != 0 && stats_monotonic_ns(&t_er1)) {
            stats_record_event_reconcile(&app.stats, t_er1 - t_er0);
        }

        if (!dirty && x11_repaint_pending(&app.x11)) dirty = true;

        if (dirty) {
            bool useful_work = x11_repaint_pending(&app.x11);
            if (!useful_work) {
                dirty = false;
                if (had_events) {
                    app.stats.skipped_presentations++;
                }
                continue;
            }

            app.stats.render_attempts++;

            uint64_t t_draw0 = 0, t_draw1 = 0;
            if (app.detailed_stats) stats_monotonic_ns(&t_draw0);
            bool rendered = render_scene(&app.render, &app.x11,
                app.x11.root_width, app.x11.root_height);
            if (app.detailed_stats && t_draw0 != 0 && stats_monotonic_ns(&t_draw1)) {
                stats_record_draw(&app.stats, t_draw1 - t_draw0);
            }
            if (!rendered) {
                dirty = false;
                if (had_events) app.stats.skipped_presentations++;
                continue;
            }

            uint64_t t_swap0 = 0, t_swap1 = 0;
            if (app.detailed_stats) stats_monotonic_ns(&t_swap0);
            if (!egl_present(&app.egl)) {
                exit_status = 1;
                goto cleanup;
            }
            if (app.detailed_stats && t_swap0 != 0 && stats_monotonic_ns(&t_swap1)) {
                stats_record_swap(&app.stats, t_swap1 - t_swap0);
            }

            app.stats.successful_swaps++;
            dirty = false;

            if (!x11_finish_fullscreen_presentation(&app.x11, &app.egl, &dirty)) {
                exit_status = 1;
                goto cleanup;
            }
        } else if (had_events) {
            app.stats.skipped_presentations++;
        }
    }

cleanup:
    if (render_initialized) {
        render_destroy(&app.render);
    }
    if (x11_initialized) {
        if (egl_initialized) {
            x11_release_windows(&app.x11, &app.egl);
            egl_destroy(&app.egl, app.x11.display);
        }
        x11_destroy(&app.x11, &app.egl);
    }

    if (app.verbose || exit_status != 0) {
        stats_dump_human(&app.stats, exit_status, STDERR_FILENO);
    }
    stats_dump(&app.stats, "exit", app.detailed_stats, STDOUT_FILENO);

    if (app.sig_pipe[0] >= 0) close(app.sig_pipe[0]);
    if (app.sig_pipe[1] >= 0) close(app.sig_pipe[1]);

    return exit_status;
}
