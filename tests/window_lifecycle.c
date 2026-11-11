#include <unistd.h>
#include <sys/wait.h>
#include "x11_stubs.h"

#define CHECK(condition, message) do { if (!(condition)) { \
    fprintf(stderr, "FAIL: %s\n", message); return 1; } } while (0)

int main(int argc, char **argv) {
    CHECK(argc == 2, "select a regression case");
    _XPrivDisplay display = calloc(1, sizeof(*display));
    CHECK(display != NULL, "allocate fake X connection");
    Stats stats;
    stats_init(&stats);
    X11State x = { .display = (Display *)display, .root = 1, .root_width = 1000, .root_height = 1000,
        .stats = &stats, .net_wm_opacity = 77, .wm_state_atom = 88, .net_wm_bypass_compositor = 99,
        .overlay = 9999, .overlay_acquired = true, .redirected = true };
    EGLState e = { .stats = &stats };

    if (!strcmp(argv[1], "trap-errors")) {
        unsigned char code = 0;
        x11_trap_errors(x.display);
        stub_emit_x_error(x.display, BadPixmap);
        CHECK(x11_untrap_errors(x.display, &code) && code == BadPixmap,
              "compatibility trap returns captured error and code");
        x11_trap_errors(x.display);
        CHECK(!x11_untrap_errors(x.display, &code) && code == 0, "fresh trap has no stale error");
        XErrorTrap trap;
        x11_trap_errors_scoped(x.display, &trap);
        stub_emit_x_error(x.display, BadDrawable);
        CHECK(x11_untrap_errors_scoped(&trap) && trap.error_code == BadDrawable && trap.resourceid == 42,
              "scoped trap retains captured details");
    } else if (!strcmp(argv[1], "damage-create-failure")) {
        x.damage_opcode = 140;
        x.damage_error_base = 143;
        x11_install_error_handler(&x);
        stub_damage_create_error = true;
        stub_damage_create_error_code = BadAlloc;
        CHECK(!x11_reconcile(&x, &e), "persistent DamageCreate failure aborts reconciliation");
        CHECK(stats.live_damage == 0 && stats.live_pixmaps == 0 && stats.live_images == 0,
              "failed DamageCreate leaves no counted or imported resources");
        stub_damage_create_error = false;
        CHECK(x11_reconcile(&x, &e), "reconciliation recovers after DamageCreate succeeds");
        CHECK(x.num_windows == 1 && x.windows[0].damage != None && stats.live_damage == 1,
              "successful DamageCreate establishes exactly one live association");
    } else if (!strcmp(argv[1], "import-failure") || !strcmp(argv[1], "vanished-import")) {
        import_ok = false;
        vanish_on_import = !strcmp(argv[1], "vanished-import");
        bool result = x11_reconcile(&x, &e);
        if (vanish_on_import) {
            CHECK(result && x.num_windows == 0, "discard a window that vanishes during import");
        } else {
            CHECK(!result, "fail reconciliation when a valid window cannot import");
        }
    } else if (!strcmp(argv[1], "partial-failure")) {
        tree_window_count = 2;
        fail_on_window_43 = true;
        bool result = x11_reconcile(&x, &e);
        CHECK(!result, "reconciliation fails if any viewable window cannot import");
        CHECK(x.num_windows == 0, "no half-initialized state retained on failure");
        CHECK(releases == 1, "first imported window is released on partial failure");
        CHECK(stats.live_pixmaps == 0, "live_pixmaps cleaned up on partial failure");
        CHECK(stats.live_images == 0, "live_images cleaned up on partial failure");
        CHECK(stats.live_textures == 0, "live_textures cleaned up on partial failure");
        CHECK(stats.live_damage == 0, "live_damage cleaned up on partial failure");
    } else if (!strcmp(argv[1], "batch-ordering")) {
        tree_window_count = 2;
        tree_windows[0] = 42;
        tree_windows[1] = 43;
        CHECK(x11_reconcile(&x, &e), "reconcile two children in batch");
        CHECK(stub_xflush_calls >= 1, "XFlush called before XCB batch");
        CHECK(stub_xcb_flush_count >= 1, "xcb_flush called after queuing requests");
        CHECK(stub_xcb_send_count == 4, "four requests queued (two per child)");
        CHECK(stub_xcb_recv_count == 4, "four replies received");
        CHECK(stub_xcb_op_count >= 9, "all dispatch, flush, and collect operations logged");
        CHECK(stub_xcb_ops[0].op == STUB_XCB_OP_ATTR_SEND && stub_xcb_ops[0].window == 42,
              "op 0: send attributes for child 42");
        CHECK(stub_xcb_ops[1].op == STUB_XCB_OP_GEOM_SEND && stub_xcb_ops[1].window == 42,
              "op 1: send geometry for child 42");
        CHECK(stub_xcb_ops[2].op == STUB_XCB_OP_ATTR_SEND && stub_xcb_ops[2].window == 43,
              "op 2: send attributes for child 43");
        CHECK(stub_xcb_ops[3].op == STUB_XCB_OP_GEOM_SEND && stub_xcb_ops[3].window == 43,
              "op 3: send geometry for child 43");
        CHECK(stub_xcb_ops[4].op == STUB_XCB_OP_FLUSH,
              "op 4: xcb_flush after all requests sent");
        CHECK(stub_xcb_ops[5].op == STUB_XCB_OP_ATTR_RECV && stub_xcb_ops[5].window == 42,
              "op 5: receive attributes for child 42");
        CHECK(stub_xcb_ops[6].op == STUB_XCB_OP_GEOM_RECV && stub_xcb_ops[6].window == 42,
              "op 6: receive geometry for child 42");
        CHECK(stub_xcb_ops[7].op == STUB_XCB_OP_ATTR_RECV && stub_xcb_ops[7].window == 43,
              "op 7: receive attributes for child 43");
        CHECK(stub_xcb_ops[8].op == STUB_XCB_OP_GEOM_RECV && stub_xcb_ops[8].window == 43,
              "op 8: receive geometry for child 43");
        CHECK(x.num_windows == 2 && x.windows[0].xid == 42 && x.windows[1].xid == 43,
              "stacking follows root tree order");
    } else if (!strcmp(argv[1], "stable-second-reconcile")) {
        tree_window_count = 1;
        tree_windows[0] = 42;
        CHECK(x11_reconcile(&x, &e), "initial reconcile");
        CHECK(x.num_windows == 1 && imports == 1, "initial import established");

        int syncs_before = stub_sync_calls;
        int attrs_before = stub_serial_attr_calls;

        CHECK(x11_reconcile(&x, &e), "stable second reconcile");
        CHECK(stub_serial_attr_calls == attrs_before,
              "zero serial XGetWindowAttributes calls during second reconcile");
        CHECK(stub_sync_calls == syncs_before + 2,
              "no per-child trap barriers during stable second reconcile (only root query barrier)");
    } else if (!strcmp(argv[1], "geometry-visuals")) {
        tree_window_count = 2;
        tree_windows[0] = 42;
        tree_windows[1] = 43;

        stub_xcb_has_custom_geom[0] = true;
        stub_xcb_window_x[0] = -50;
        stub_xcb_window_y[0] = 100;
        stub_xcb_window_w[0] = 320;
        stub_xcb_window_h[0] = 240;
        stub_xcb_window_bw[0] = 3;
        stub_xcb_window_visual_id[42 % 16] = 24;

        stub_xcb_has_custom_geom[1] = true;
        stub_xcb_window_x[1] = 200;
        stub_xcb_window_y[1] = 300;
        stub_xcb_window_w[1] = 400;
        stub_xcb_window_h[1] = 300;
        stub_xcb_window_bw[1] = 0;
        stub_xcb_window_visual_id[43 % 16] = 32;

        CHECK(x11_reconcile(&x, &e), "reconcile custom geometries and visuals");
        CHECK(x.num_windows == 2, "both windows reconciled");
        CHECK(x.windows[0].x == -50 && x.windows[0].y == 100 && x.windows[0].border_width == 3,
              "w42: negative position and border width preserved");
        CHECK(x.windows[0].visual_id == 24 && !x.windows[0].has_alpha,
              "w42: 24-bit RGB visual without alpha");
        CHECK(x.windows[1].x == 200 && x.windows[1].y == 300 && x.windows[1].border_width == 0,
              "w43: position and zero border preserved");
        CHECK(x.windows[1].visual_id == 32 && x.windows[1].has_alpha,
              "w43: 32-bit ARGB visual with alpha classification preserved");
    } else if (!strcmp(argv[1], "subscription-preservation")) {
        tree_window_count = 1;
        tree_windows[0] = 42;
        stub_window_masks[0] = KeyPressMask | ExposureMask;
        CHECK(x11_reconcile(&x, &e), "reconcile preserving subscriptions");
        long expected = KeyPressMask | ExposureMask | PropertyChangeMask |
                        StructureNotifyMask | SubstructureNotifyMask;
        CHECK((stub_window_masks[0] & expected) == expected,
              "existing per-connection event masks preserved alongside compositor subscriptions");
    } else if (!strcmp(argv[1], "vanished-child")) {
        tree_window_count = 2;
        tree_windows[0] = 42;
        tree_windows[1] = 43;

        /* Test A: Attributes error on window 42 */
        stub_xcb_fail_attr_window = 42;
        stub_xcb_attr_error_code = BadWindow;
        CHECK(x11_reconcile(&x, &e), "reconcile survives attributes error on child 42");
        CHECK(x.num_windows == 1 && x.windows[0].xid == 43,
              "surviving sibling 43 reconciled");
        CHECK(stub_xcb_recv_count == 4, "all queued requests consumed after attr error");
        x11_release_windows(&x, &e);

        /* Test B: Geometry error on window 43 */
        reset_x11_stubs();
        tree_window_count = 2;
        tree_windows[0] = 42;
        tree_windows[1] = 43;
        stub_xcb_fail_geom_window = 43;
        stub_xcb_geom_error_code = BadDrawable;
        CHECK(x11_reconcile(&x, &e), "reconcile survives geometry error on child 43");
        CHECK(x.num_windows == 1 && x.windows[0].xid == 42,
              "surviving sibling 42 reconciled");
        CHECK(stub_xcb_recv_count == 4, "all queued requests consumed after geom error");
    } else if (!strcmp(argv[1], "filtered-empty-children")) {
        tree_window_count = 4;
        tree_windows[0] = 42;
        tree_windows[1] = 43;
        tree_windows[2] = 44;
        tree_windows[3] = 999;
        x.overlay = 999;
        stub_xcb_input_only_window = 43;
        stub_xcb_unmapped_window = 44;

        CHECK(x11_reconcile(&x, &e), "reconcile tree with mixed window types");
        CHECK(x.num_windows == 1 && x.windows[0].xid == 42,
              "only viewable InputOutput non-overlay window 42 captured");
        CHECK(stub_xcb_send_count == 6, "overlay 999 excluded from batch (3 windows queued * 2 = 6)");

        /* Test empty tree */
        x11_release_windows(&x, &e);
        tree_window_count = 0;
        CHECK(x11_reconcile(&x, &e), "empty tree reconciles cleanly");
        CHECK(x.num_windows == 0, "0 windows retained for empty tree");
    } else if (!strcmp(argv[1], "failure-after-collection")) {
        tree_window_count = 2;
        tree_windows[0] = 42;
        tree_windows[1] = 43;
        fail_on_window_43 = true;

        CHECK(!x11_reconcile(&x, &e), "reconcile fails when viewable child fails import");
        CHECK(stub_xcb_send_count == 4 && stub_xcb_recv_count == 4,
              "all batch requests consumed before failure");
        CHECK(stats.live_pixmaps == 0 && stats.live_images == 0 &&
              stats.live_textures == 0 && stats.live_damage == 0,
              "all acquired resources cleaned up on failure after collection");
    } else if (!strcmp(argv[1], "xcb-protocol-errors")) {
        for (int geometry = 0; geometry < 2; ++geometry) {
            for (int unexpected = 0; unexpected < 2; ++unexpected) {
                reset_x11_stubs();
                tree_window_count = 2;
                if (geometry) {
                    stub_xcb_fail_geom_window = 42;
                    stub_xcb_geom_error_code = unexpected ? BadAlloc : BadDrawable;
                } else {
                    stub_xcb_fail_attr_window = 42;
                    stub_xcb_attr_error_code = unexpected ? BadAlloc : BadWindow;
                }
                FILE *log = tmpfile();
                CHECK(log != NULL, "capture query diagnostics");
                fflush(stderr);
                int saved_stderr = dup(STDERR_FILENO);
                CHECK(saved_stderr >= 0 && dup2(fileno(log), STDERR_FILENO) >= 0,
                      "redirect diagnostics to temporary file");
                bool reconciled = x11_reconcile(&x, &e);
                fflush(stderr);
                CHECK(dup2(saved_stderr, STDERR_FILENO) >= 0, "restore diagnostics");
                close(saved_stderr);
                rewind(log);
                char message[512] = {0};
                size_t length = fread(message, 1, sizeof(message) - 1, log);
                fclose(log);
                CHECK(reconciled && x.num_windows == 1 && x.windows[0].xid == 43,
                      "query failure skips only the affected child");
                if (unexpected) {
                    CHECK(strstr(message, geometry ? "XCB GetGeometry" : "XCB GetWindowAttributes") &&
                          strstr(message, "window 0x2a") && strstr(message, "error 11") &&
                          strstr(message, "request ") && strstr(message, "sequence "),
                          "unexpected error identifies query, window, code, and request");
                } else {
                    CHECK(length == 0, "expected vanished-window errors remain quiet");
                }
                CHECK(stub_xcb_outstanding == 0, "error replies consume all queued requests");
                x11_release_windows(&x, &e);
            }
        }
    } else if (!strcmp(argv[1], "xcb-reply-lifecycle")) {
        xcb_connection_t *connection = XGetXCBConnection(x.display);
        /* Exercise 32-bit cookie rollover while Xlib keeps its wider serial. */
        display->request = UINT32_MAX - 1UL;
        XSync(x.display, False);
        unsigned long next_serial = NextRequest(x.display);
        xcb_get_window_attributes_cookie_t attributes = xcb_get_window_attributes(connection, 42);
        xcb_get_geometry_cookie_t geometry = xcb_get_geometry(connection, 42);
        CHECK(attributes.sequence == (unsigned int)next_serial &&
              geometry.sequence == (unsigned int)(next_serial + 1) &&
              NextRequest(x.display) == next_serial + 2,
              "XCB requests share Xlib request serials across cookie rollover");
        CHECK(stub_xcb_outstanding == 2, "both queued replies are outstanding");
        xcb_generic_error_t *error = NULL;
        CHECK(xcb_get_geometry_reply(connection,
                  (xcb_get_geometry_cookie_t){ attributes.sequence }, &error) == NULL &&
              error == NULL && stub_xcb_outstanding == 2,
              "wrong reply type is rejected without consuming another request");
        void *reply = xcb_get_geometry_reply(connection, geometry, &error);
        CHECK(reply && !error && stub_xcb_outstanding == 1, "geometry reply consumed once");
        free(reply);
        reply = xcb_get_window_attributes_reply(connection, attributes, &error);
        CHECK(reply && !error && stub_xcb_outstanding == 0, "attributes reply consumed once");
        free(reply);
        CHECK(xcb_get_window_attributes_reply(connection, attributes, &error) == NULL &&
              stub_xcb_invalid_replies == 2 && stub_xcb_recv_count == 2,
              "duplicate cookie rejected without manufacturing another reply");
        stub_xcb_invalid_replies = 0; /* Invalid retrievals above are intentional. */

        CHECK(x11_reconcile(&x, &e), "capture window for mixed-stream error delivery");
        x.damage_opcode = 140;
        x.damage_error_base = 143;
        x11_install_error_handler(&x);
        x.windows[0].is_occluded = true;
        stub_damage_subtract_error = true;
        stub_error_damage_id = x.windows[0].damage;
        XEvent event = {0};
        XDamageNotifyEvent *damage = (XDamageNotifyEvent *)&event;
        damage->type = x.damage_event_base + XDamageNotify;
        damage->damage = x.windows[0].damage;
        bool dirty = false, stop = false;
        x11_handle_event(&x, &event, &dirty, &stop);
        CHECK(x.damage_request_count == 1 && !x.damage_requests[0].failed,
              "earlier hidden Damage error is pending before XCB reply");
        attributes = xcb_get_window_attributes(connection, 42);
        reply = xcb_get_window_attributes_reply(connection, attributes, &error);
        CHECK(reply && !error && x.damage_requests[0].failed && x.windows[0].damage != None,
              "XCB reply delivers earlier Xlib errors without callback-side resource retirement");
        free(reply);
        x11_finish_damage_batch(&x);
        CHECK(x.damage_request_count == 0 && x.windows[0].damage == None,
              "normal Damage completion retires the failed association");
    } else if (!strcmp(argv[1], "connection-loss")) {
        for (int phase = 0; phase < 3; ++phase) {
            pid_t pid = fork();
            CHECK(pid >= 0, "fork child for connection loss test");
            if (pid == 0) {
                reset_x11_stubs();
                stub_xcb_connection_error = phase == 0;
                stub_xcb_flush_failure = phase == 1;
                stub_xcb_disconnect_after_replies = phase == 2 ? 1 : 0;
                x11_reconcile(&x, &e);
                _exit(0); /* Fatal handling must exit; dead Xlib calls exit 99. */
            }
            int status_val = 0;
            CHECK(waitpid(pid, &status_val, 0) == pid, "wait for child process");
            CHECK(WIFEXITED(status_val) && WEXITSTATUS(status_val) == 1,
                  "preflight, flush, and reply loss exit fatally without dead-connection cleanup");
        }
    } else if (!strcmp(argv[1], "fullscreen-policy-invalidation")) {
        x.unredir_fullscreen = true;
        x.fullscreen_recheck = true;
        tree_window_count = 1;
        tree_windows[0] = 42;
        window_x = window_y = 0;
        client_width = client_height = 1000;
        border_width = 0;
        stub_frame_has_wm_state = true;
        stub_bypass_hint_set = true;
        stub_bypass_hint_value = 2;
        CHECK(x11_reconcile(&x, &e), "reconcile initially vetoed candidate");

        bool dirty = false, stop = false;
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
              "evaluate initial fullscreen policy");
        CHECK(x.fullscreen_mode == FULLSCREEN_COMPOSITED && !x.fullscreen_recheck,
              "initial ineligible policy result is consumed");
        int syncs_after_policy = stub_sync_calls;
        uint64_t queries_after_policy = stats.x_round_trips;
        for (int i = 0; i < 10; ++i) {
            CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
                  "advance unchanged composited policy");
        }
        CHECK(stub_sync_calls == syncs_after_policy && stats.x_round_trips == queries_after_policy,
              "unchanged composited iterations do not repeat eligibility queries");

        XEvent event = {0};
        event.type = x.damage_event_base + XDamageNotify;
        ((XDamageNotifyEvent *)&event)->damage = 999;
        x11_handle_event(&x, &event, &dirty, &stop);
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
              "advance after unrelated Damage event");
        CHECK(stub_sync_calls == syncs_after_policy && stats.x_round_trips == queries_after_policy,
              "Damage-only wakeup does not repeat eligibility queries");

        x.fullscreen_recheck = true;
        x.pending_changes = CHANGE_HIERARCHY;
        stub_query_tree_fail = true;
        CHECK(!x11_reconcile(&x, &e),
              "failed root QueryTree cannot authorize policy against stale topology");
        CHECK(x.fullscreen_recheck, "failed scene query does not consume policy invalidation");
        stub_query_tree_fail = false;
        x.pending_changes = CHANGE_NONE;
        x.fullscreen_recheck = false;
        syncs_after_policy = stub_sync_calls;
        queries_after_policy = stats.x_round_trips;

        stub_bypass_hint_value = 0;
        memset(&event, 0, sizeof(event));
        event.type = PropertyNotify;
        event.xproperty.window = 42;
        event.xproperty.atom = x.net_wm_bypass_compositor;
        x11_handle_event(&x, &event, &dirty, &stop);
        CHECK(x.fullscreen_recheck && x.pending_reconcile,
              "relevant property event schedules policy and scene work");
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
              "defer policy until pending scene work is consumed");
        CHECK(x.fullscreen_recheck && stats.x_round_trips == queries_after_policy,
              "deferred evaluation retains its invalidation without querying");
        x.pending_reconcile = false; /* Main clears this immediately before reconcile. */
        CHECK(x11_reconcile(&x, &e), "consume pending property reconciliation");
        syncs_after_policy = stub_sync_calls;
        queries_after_policy = stats.x_round_trips;

        stub_pending_events = 1;
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
              "defer when an X event is already queued");
        CHECK(x.fullscreen_recheck && stats.x_round_trips == queries_after_policy,
              "queued X work retains its invalidation");
        stub_pending_events = 0;
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
              "evaluate after queued X work is consumed");
        CHECK(x.fullscreen_mode == FULLSCREEN_BYPASSED && !x.fullscreen_recheck,
              "new candidate remains bypassed without a stale restoration request");

        syncs_after_policy = stub_sync_calls;
        queries_after_policy = stats.x_round_trips;
        for (int i = 0; i < 10; ++i) {
            CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
                  "advance unchanged bypassed policy");
        }
        CHECK(x.fullscreen_mode == FULLSCREEN_BYPASSED &&
              stub_sync_calls == syncs_after_policy && stats.x_round_trips == queries_after_policy,
              "unchanged bypassed iterations neither restore nor repeat eligibility queries");

        /* Shape-only changes to a rejected distinct client must wake policy. */
        x11_release_windows(&x, &e);
        reset_x11_stubs();
        x.fullscreen_mode = FULLSCREEN_COMPOSITED;
        x.unredir_fullscreen = true;
        x.fullscreen_recheck = true;
        x.pending_reconcile = false;
        x.pending_changes = CHANGE_NONE;
        x.root_size_changed = false;
        x.redirected = true;
        x.overlay_hidden = false;
        x.bypass_frame = None;
        x.bypass_client = None;
        x.shape_event_base = 60;
        tree_window_count = 1;
        tree_windows[0] = 42;
        window_x = window_y = 0;
        client_width = client_height = 1000;
        border_width = 0;
        stub_frame_client = 43;
        stub_client_shaped = true;
        stub_opacity_set = true;
        stub_opacity_value = 0xffffffffUL;
        CHECK(x11_reconcile(&x, &e), "reconcile fullscreen frame with shaped distinct client");
        CHECK(x.windows[0].opacity_client == 43, "discover distinct fullscreen client");
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
              "evaluate initially shaped client");
        CHECK(x.fullscreen_mode == FULLSCREEN_COMPOSITED && !x.fullscreen_recheck,
              "initially shaped client is ineligible for bypass");
        CHECK(stub_shape_masks[1] & ShapeNotifyMask,
              "rejected distinct client is watched for shape changes");

        stub_client_shaped = false;
        memset(&event, 0, sizeof(event));
        event.type = x.shape_event_base + ShapeNotify;
        ((XShapeEvent *)&event)->window = 43;
        ((XShapeEvent *)&event)->kind = ShapeBounding;
        x11_handle_event(&x, &event, &dirty, &stop);
        CHECK(x.fullscreen_recheck && !x.pending_reconcile,
              "client ShapeNotify schedules policy without scene reconciliation");
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
              "reevaluate policy after the shape-only event");
        CHECK(x.fullscreen_mode == FULLSCREEN_BYPASSED && x.bypass_client == 43,
              "rectangular client enters bypass through ShapeNotify");

        stub_frame_client = 44;
        memset(&event, 0, sizeof(event));
        event.type = ReparentNotify;
        event.xreparent.window = 44;
        x11_handle_event(&x, &event, &dirty, &stop);
        x.pending_reconcile = false; /* Main clears this before reconciliation. */
        CHECK(x11_reconcile(&x, &e), "reconcile replacement fullscreen client");
        CHECK(x.windows[0].opacity_client == 44, "discover replacement fullscreen client");
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
              "reevaluate policy for replacement client");
        CHECK(stub_shape_masks[2] & ShapeNotifyMask,
              "replacement distinct client is watched for shape changes");
    } else if (!strcmp(argv[1], "fullscreen-acceptance")) {
        x.unredir_fullscreen = true;
        x.fullscreen_recheck = true;
        tree_window_count = 1;
        tree_windows[0] = 42;
        window_x = window_y = 0;
        client_width = client_height = 1000;
        border_width = 0;
        stub_frame_has_wm_state = true;

        CHECK(x11_reconcile(&x, &e), "reconcile fullscreen candidate window");
        CHECK(x.num_windows == 1 && imports == 1, "fullscreen window imported");

        bool dirty = false;
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty), "advance to bypass");
        CHECK(x.fullscreen_mode == FULLSCREEN_BYPASSED, "mode is FULLSCREEN_BYPASSED");
        CHECK(x.bypass_frame == 42 && x.bypass_client == 42, "bypass frame and client set");
        CHECK(!x.redirected, "root subwindows unredirected");
        CHECK(x.overlay_hidden, "overlay marked hidden");
        CHECK(stub_composite_unredirect_calls == 1, "XCompositeUnredirectSubwindows called once");
        CHECK(stub_shape_set_bounding_calls == 1, "bounding shape hidden once");
        CHECK(releases == 1, "storage released on bypass entry");
        CHECK(stats.live_images == 0 && stats.live_textures == 0 &&
              stats.live_pixmaps == 0 && stats.live_damage == 0,
              "zero live storage resources while bypassed");

        /* Candidate with distinct client and hint 1 */
        reset_x11_stubs();
        x.fullscreen_mode = FULLSCREEN_COMPOSITED;
        x.fullscreen_recheck = true;
        x.redirected = true;
        x.overlay_hidden = false;
        x11_release_windows(&x, &e);
        tree_window_count = 1;
        tree_windows[0] = 42;
        window_x = window_y = 0;
        client_width = client_height = 1000;
        border_width = 0;
        stub_frame_client = 43;
        stub_client_has_custom_geom = true;
        stub_client_w = stub_client_h = 1000;
        stub_client_border_width = 0;
        stub_translate_dest_x = stub_translate_dest_y = 0;
        stub_bypass_hint_set = true;
        stub_bypass_hint_value = 1;
        stub_opacity_set = true;
        stub_opacity_value = 0xffffffffUL;
        CHECK(x11_reconcile(&x, &e), "reconcile frame 42 with client 43");
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty), "advance bypass with distinct client");
        CHECK(x.fullscreen_mode == FULLSCREEN_BYPASSED, "mode is FULLSCREEN_BYPASSED (second)");
        CHECK(x.bypass_frame == 42 && x.bypass_client == 43, "bypass frame is 42, client is 43");
    } else if (!strcmp(argv[1], "fullscreen-rejections")) {
        x.unredir_fullscreen = true;
        tree_window_count = 1;
        tree_windows[0] = 42;

        struct RejectionCase {
            const char *desc;
            int x_pos, y_pos, w, h, bw;
            VisualID vid;
            bool shaped;
            bool opacity_set;
            unsigned long opacity_val;
            bool opacity_malformed;
            bool hint_set;
            unsigned long hint_val;
            bool hint_malformed;
            Window frame_client;
            int client_bw;
            int trans_x, trans_y;
            VisualID client_vid;
            bool client_shaped;
            bool frame_has_wm_state;
            bool override_redirect;
        } cases[] = {
            { "partial x position", 10, 0, 1000, 1000, 0, 24, false, false, 0, false, false, 0, false, None, 0, 0, 0, 24, false, true, false },
            { "partial y position", 0, 10, 1000, 1000, 0, 24, false, false, 0, false, false, 0, false, None, 0, 0, 0, 24, false, true, false },
            { "sub-root width", 0, 0, 999, 1000, 0, 24, false, false, 0, false, false, 0, false, None, 0, 0, 0, 24, false, true, false },
            { "sub-root height", 0, 0, 1000, 999, 0, 24, false, false, 0, false, false, 0, false, None, 0, 0, 0, 24, false, true, false },
            { "nonzero border", 0, 0, 1000, 1000, 1, 24, false, false, 0, false, false, 0, false, None, 0, 0, 0, 24, false, true, false },
            { "ARGB visual", 0, 0, 1000, 1000, 0, 32, false, false, 0, false, false, 0, false, None, 0, 0, 0, 24, false, true, false },
            { "shaped frame", 0, 0, 1000, 1000, 0, 24, true, false, 0, false, false, 0, false, None, 0, 0, 0, 24, false, true, false },
            { "opacity < UINT32_MAX", 0, 0, 1000, 1000, 0, 24, false, true, 0xfffffffeUL, false, false, 0, false, None, 0, 0, 0, 24, false, true, false },
            { "malformed opacity", 0, 0, 1000, 1000, 0, 24, false, true, 0, true, false, 0, false, None, 0, 0, 0, 24, false, true, false },
            { "bypass hint veto = 2", 0, 0, 1000, 1000, 0, 24, false, false, 0, false, true, 2, false, None, 0, 0, 0, 24, false, true, false },
            { "malformed bypass hint", 0, 0, 1000, 1000, 0, 24, false, false, 0, false, true, 0, true, None, 0, 0, 0, 24, false, true, false },
            { "client nonzero border", 0, 0, 1000, 1000, 0, 24, false, false, 0, false, false, 0, false, 43, 1, 0, 0, 24, false, false, false },
            { "client translated offset", 0, 0, 1000, 1000, 0, 24, false, false, 0, false, false, 0, false, 43, 0, 5, 0, 24, false, false, false },
            { "client ARGB visual", 0, 0, 1000, 1000, 0, 24, false, false, 0, false, false, 0, false, 43, 0, 0, 0, 32, false, false, false },
            { "client shaped", 0, 0, 1000, 1000, 0, 24, false, false, 0, false, false, 0, false, 43, 0, 0, 0, 24, true, false, false },
            { "missing WM_STATE on unframed root child", 0, 0, 1000, 1000, 0, 24, false, false, 0, false, false, 0, false, None, 0, 0, 0, 24, false, false, false },
            { "override-redirect root child", 0, 0, 1000, 1000, 0, 24, false, false, 0, false, false, 0, false, None, 0, 0, 0, 24, false, true, true },
        };

        for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
            reset_x11_stubs();
            x.fullscreen_mode = FULLSCREEN_COMPOSITED;
            x.fullscreen_recheck = true;
            x.redirected = true;
            x.overlay_hidden = false;
            x11_release_windows(&x, &e);

            window_x = cases[i].x_pos;
            window_y = cases[i].y_pos;
            client_width = cases[i].w;
            client_height = cases[i].h;
            border_width = cases[i].bw;
            stub_xcb_window_visual_id[42 % 16] = cases[i].vid;
            empty_shape = cases[i].shaped;
            stub_opacity_set = cases[i].opacity_set;
            stub_opacity_value = cases[i].opacity_val;
            stub_opacity_malformed = cases[i].opacity_malformed;
            stub_bypass_hint_set = cases[i].hint_set;
            stub_bypass_hint_value = cases[i].hint_val;
            stub_bypass_hint_malformed = cases[i].hint_malformed;
            stub_frame_client = cases[i].frame_client;
            stub_frame_has_wm_state = cases[i].frame_has_wm_state;
            stub_xcb_override_redirect_window = cases[i].override_redirect ? 42 : None;
            stub_client_has_custom_geom = (cases[i].frame_client != None);
            stub_client_w = 1000; stub_client_h = 1000;
            stub_client_border_width = cases[i].client_bw;
            stub_translate_dest_x = cases[i].trans_x;
            stub_translate_dest_y = cases[i].trans_y;
            stub_client_visual_id = cases[i].client_vid;
            stub_client_shaped = cases[i].client_shaped;

            CHECK(x11_reconcile(&x, &e), "reconcile rejection case");
            bool dirty = false;
            CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty), "advance rejection case");
            CHECK(x.fullscreen_mode == FULLSCREEN_COMPOSITED, cases[i].desc);
            CHECK(x.redirected, "remains redirected on rejection");
            CHECK(!x.overlay_hidden, "overlay remains visible on rejection");
        }
    } else if (!strcmp(argv[1], "fullscreen-popups")) {
        x.unredir_fullscreen = true;
        x.fullscreen_recheck = true;
        tree_window_count = 2;
        tree_windows[0] = 42;
        tree_windows[1] = 43;
        stub_frame_has_wm_state = true;

        stub_xcb_has_custom_geom[0] = true;
        stub_xcb_window_x[0] = 0; stub_xcb_window_y[0] = 0;
        stub_xcb_window_w[0] = 1000; stub_xcb_window_h[0] = 1000;
        stub_xcb_window_bw[0] = 0;
        stub_xcb_window_visual_id[42 % 16] = 24;

        stub_xcb_has_custom_geom[1] = true;
        stub_xcb_window_x[1] = 100; stub_xcb_window_y[1] = 100;
        stub_xcb_window_w[1] = 200; stub_xcb_window_h[1] = 200;
        stub_xcb_window_bw[1] = 0;
        stub_xcb_window_visual_id[43 % 16] = 24;
        stub_xcb_override_redirect_window = 43;

        /* Case 1: Visible override-redirect popup blocks bypass */
        CHECK(x11_reconcile(&x, &e), "reconcile fullscreen with popup");
        CHECK(x.num_windows == 2, "both windows in scene");
        CHECK(x.windows[1].override_redirect, "w43 has override_redirect set");
        bool dirty = false;
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty), "advance mode with popup");
        CHECK(x.fullscreen_mode == FULLSCREEN_COMPOSITED, "popup above fullscreen blocks bypass");

        /* Case 2: Border is already included in outer geometry. This popup's
         * actual right edge is -10, so it must not block the root candidate. */
        stub_xcb_window_x[1] = -130; stub_xcb_window_y[1] = 100;
        stub_xcb_window_w[1] = 80; stub_xcb_window_h[1] = 200;
        stub_xcb_window_bw[1] = 20;
        x.windows[1].storage_dirty = true;
        CHECK(x11_reconcile(&x, &e), "reconcile bordered offscreen popup");
        CHECK(x.windows[1].width == 120 && x.windows[1].height == 240,
              "composited popup geometry already includes its border");
        x.fullscreen_recheck = true;
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
              "advance mode with bordered offscreen popup");
        CHECK(x.fullscreen_mode == FULLSCREEN_BYPASSED,
              "bordered popup outside the root does not block bypass");

        CHECK(x11_reconcile(&x, &e), "reconcile bordered popup in metadata-only mode");
        CHECK(x.windows[1].width == 120 && x.windows[1].height == 240,
              "metadata-only popup geometry includes its border once");
        x.fullscreen_recheck = true;
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
              "reevaluate bordered popup in metadata-only mode");
        CHECK(x.fullscreen_mode == FULLSCREEN_BYPASSED,
              "metadata-only outer bounds do not count popup border twice");

        /* Case 3: Unmapped top window does not block bypass */
        reset_x11_stubs();
        x.fullscreen_mode = FULLSCREEN_COMPOSITED;
        x.fullscreen_recheck = true;
        x.redirected = true;
        x.overlay_hidden = false;
        x11_release_windows(&x, &e);
        stub_frame_has_wm_state = true;
        stub_xcb_has_custom_geom[0] = true;
        stub_xcb_window_x[0] = 0; stub_xcb_window_y[0] = 0;
        stub_xcb_window_w[0] = 1000; stub_xcb_window_h[0] = 1000;
        stub_xcb_window_bw[0] = 0;
        stub_xcb_unmapped_window = 43;
        CHECK(x11_reconcile(&x, &e), "reconcile with unmapped top window");
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty), "advance mode with unmapped window");
        CHECK(x.fullscreen_mode == FULLSCREEN_BYPASSED, "unmapped top window does not block bypass");

        /* Case 4: InputOnly top window does not block bypass */
        reset_x11_stubs();
        x.fullscreen_mode = FULLSCREEN_COMPOSITED;
        x.fullscreen_recheck = true;
        x.redirected = true;
        x.overlay_hidden = false;
        x11_release_windows(&x, &e);
        stub_frame_has_wm_state = true;
        stub_xcb_has_custom_geom[0] = true;
        stub_xcb_window_x[0] = 0; stub_xcb_window_y[0] = 0;
        stub_xcb_window_w[0] = 1000; stub_xcb_window_h[0] = 1000;
        stub_xcb_window_bw[0] = 0;
        stub_xcb_input_only_window = 43;
        CHECK(x11_reconcile(&x, &e), "reconcile with InputOnly top window");
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty), "advance mode with InputOnly window");
        CHECK(x.fullscreen_mode == FULLSCREEN_BYPASSED, "InputOnly top window does not block bypass");
    } else if (!strcmp(argv[1], "fullscreen-bypassed-reconcile")) {
        x.unredir_fullscreen = true;
        x.fullscreen_recheck = true;
        tree_window_count = 1;
        tree_windows[0] = 42;
        window_x = window_y = 0;
        client_width = client_height = 1000;
        border_width = 0;
        stub_frame_has_wm_state = true;

        CHECK(x11_reconcile(&x, &e), "initial reconcile");
        bool dirty = false;
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty), "enter bypass");
        CHECK(x.fullscreen_mode == FULLSCREEN_BYPASSED, "in bypass mode");

        int initial_imports = imports;
        int initial_pixmap_frees = free_pixmap_calls;

        /* Reconcile while bypassed: metadata only */
        CHECK(x11_reconcile(&x, &e), "reconcile while bypassed");
        CHECK(imports == initial_imports, "zero imports performed while bypassed");
        CHECK(free_pixmap_calls == initial_pixmap_frees, "zero pixmap frees while bypassed");
        CHECK(stats.live_damage == 0 && stats.live_pixmaps == 0, "zero live storage while bypassed");
        CHECK(x.num_windows == 1 && x.windows[0].width == 1000 && x.windows[0].height == 1000,
              "outer dimensions contract preserved");

        /* Add a bordered window during bypass */
        tree_window_count = 2;
        tree_windows[1] = 43;
        stub_xcb_has_custom_geom[1] = true;
        stub_xcb_window_x[1] = 50; stub_xcb_window_y[1] = 50;
        stub_xcb_window_w[1] = 200; stub_xcb_window_h[1] = 100;
        stub_xcb_window_bw[1] = 4;
        CHECK(x11_reconcile(&x, &e), "reconcile second bordered window while bypassed");
        CHECK(x.num_windows == 2, "both windows present in metadata");
        CHECK(x.windows[1].width == 208 && x.windows[1].height == 108,
              "bordered window outer bounds include 2 * border_width");
        CHECK(x.windows[1].pixmap == None && x.windows[1].imported.image == EGL_NO_IMAGE_KHR,
              "new window has no storage while bypassed");
        CHECK(imports == initial_imports, "still zero imports while bypassed");
    } else if (!strcmp(argv[1], "fullscreen-two-phase-restoration")) {
        x.unredir_fullscreen = true;
        x.fullscreen_recheck = true;
        tree_window_count = 1;
        tree_windows[0] = 42;
        window_x = window_y = 0;
        client_width = client_height = 1000;
        border_width = 0;
        stub_frame_has_wm_state = true;

        CHECK(x11_reconcile(&x, &e), "initial reconcile");
        bool dirty = false;
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty), "enter bypass");
        CHECK(x.fullscreen_mode == FULLSCREEN_BYPASSED, "bypassed");
        CHECK(stub_composite_unredirect_calls == 1 && !x.redirected, "unredirected");

        /* Invalidate candidate by resizing it */
        client_width = 500;
        CHECK(x11_reconcile(&x, &e), "update bypassed geometry before restoration");
        x.fullscreen_recheck = true;
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty), "advance mode to trigger restoration");

        /* Restoration Phase 1: FULLSCREEN_RESTORE_HIDDEN */
        CHECK(x.fullscreen_mode == FULLSCREEN_RESTORE_HIDDEN, "entered FULLSCREEN_RESTORE_HIDDEN");
        CHECK(x.redirected, "reacquired root redirection");
        CHECK(stub_composite_redirect_calls == 1, "XCompositeRedirectSubwindows called");
        CHECK(x.overlay_hidden, "overlay remains hidden during phase 1");
        CHECK(dirty && x.needs_full_repaint, "full repaint scheduled for hidden swap");
        CHECK(imports == 2, "storage rebuilt for scene");
        CHECK(stats.live_pixmaps == 1 && stats.live_images == 1, "resources restored");

        /* Simulate Phase 1 presentation completion (hidden full swap) */
        CHECK(x11_finish_fullscreen_presentation(&x, &e, &dirty), "finish presentation phase 1");

        /* Restoration Phase 2: FULLSCREEN_RESTORE_VISIBLE */
        CHECK(x.fullscreen_mode == FULLSCREEN_RESTORE_VISIBLE, "entered FULLSCREEN_RESTORE_VISIBLE");
        CHECK(!x.overlay_hidden, "overlay revealed");
        CHECK(stub_shape_set_bounding_calls == 2 && stub_last_bounding_shape_region == None,
              "bounding shape restored to None (revealed)");
        CHECK(dirty && x.needs_full_repaint, "visible full repair frame scheduled");

        /* Simulate Phase 2 presentation completion (visible repair swap) */
        CHECK(x11_finish_fullscreen_presentation(&x, &e, &dirty), "finish presentation phase 2");
        CHECK(x.fullscreen_mode == FULLSCREEN_COMPOSITED, "returned to FULLSCREEN_COMPOSITED");
        CHECK(x.bypass_frame == None && x.bypass_client == None, "bypass identifiers cleared");
    } else if (!strcmp(argv[1], "fullscreen-intervening-event")) {
        x.unredir_fullscreen = true;
        x.fullscreen_recheck = true;
        tree_window_count = 1;
        tree_windows[0] = 42;
        window_x = window_y = 0;
        client_width = client_height = 1000;
        border_width = 0;
        stub_frame_has_wm_state = true;

        CHECK(x11_reconcile(&x, &e), "initial reconcile");
        bool dirty = false;
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty), "enter bypass");

        /* Trigger restoration */
        client_width = 800;
        CHECK(x11_reconcile(&x, &e), "update bypassed geometry before restore");
        x.fullscreen_recheck = true;
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty), "start restore");
        CHECK(x.fullscreen_mode == FULLSCREEN_RESTORE_HIDDEN, "in RESTORE_HIDDEN");

        /* Intervening event arrives while in RESTORE_HIDDEN */
        XEvent ev = {0};
        ev.type = ConfigureNotify;
        ev.xconfigure.window = 42;
        ev.xconfigure.width = 1000;
        ev.xconfigure.height = 1000;
        bool stop = false;
        x11_handle_event(&x, &ev, &dirty, &stop);
        CHECK(x.fullscreen_recheck, "recheck set by event");

        /* Attempt to advance fullscreen while in RESTORE_HIDDEN */
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty), "advance while in RESTORE_HIDDEN");
        CHECK(x.fullscreen_mode == FULLSCREEN_RESTORE_HIDDEN,
              "restoration phase cannot be interrupted by bypass re-entry");

        /* Complete handoff */
        CHECK(x11_finish_fullscreen_presentation(&x, &e, &dirty), "phase 1 presentation");
        CHECK(x.fullscreen_mode == FULLSCREEN_RESTORE_VISIBLE, "in RESTORE_VISIBLE");
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty), "advance while in RESTORE_VISIBLE");
        CHECK(x.fullscreen_mode == FULLSCREEN_RESTORE_VISIBLE,
              "RESTORE_VISIBLE cannot be interrupted");
        CHECK(x11_finish_fullscreen_presentation(&x, &e, &dirty), "phase 2 presentation");
        CHECK(x.fullscreen_mode == FULLSCREEN_COMPOSITED, "reached COMPOSITED");
    } else if (!strcmp(argv[1], "fullscreen-failures")) {
        x.unredir_fullscreen = true;
        x.fullscreen_recheck = true;
        tree_window_count = 1;
        tree_windows[0] = 42;
        window_x = window_y = 0;
        client_width = client_height = 1000;
        border_width = 0;
        stub_frame_has_wm_state = true;

        /* Case 1: Failure in XCompositeUnredirectSubwindows */
        CHECK(x11_reconcile(&x, &e), "reconcile");
        stub_composite_unredirect_fail = true;
        bool dirty = false;
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
              "advance with unredirect failure");
        CHECK(!x.unredir_fullscreen, "bypass disabled for process on unredirect failure");
        CHECK(x.fullscreen_mode == FULLSCREEN_COMPOSITED, "mode remains COMPOSITED");
        CHECK(x.redirected, "redirection maintained");
        CHECK(x.windows[0].imported.image != EGL_NO_IMAGE_KHR && !x.pending_reconcile,
              "client storage is rebuilt before failed-entry rendering");

        /* Case 2: Failure in overlay hiding */
        reset_x11_stubs();
        x.unredir_fullscreen = true;
        x.fullscreen_recheck = true;
        x.fullscreen_mode = FULLSCREEN_COMPOSITED;
        x.redirected = true;
        x.overlay_hidden = false;
        x11_release_windows(&x, &e);
        tree_window_count = 1;
        tree_windows[0] = 42;
        window_x = window_y = 0;
        client_width = client_height = 1000;
        border_width = 0;
        stub_frame_has_wm_state = true;
        CHECK(x11_reconcile(&x, &e), "reconcile for overlay hide failure");
        stub_shape_region_fail = true;
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
              "advance with overlay hide failure");
        CHECK(!x.unredir_fullscreen, "bypass disabled for process on overlay hide failure");
        CHECK(x.fullscreen_mode == FULLSCREEN_COMPOSITED, "mode remains COMPOSITED");
        CHECK(x.redirected, "redirection recovered after overlay hide failure");
        CHECK(x.windows[0].imported.image != EGL_NO_IMAGE_KHR && !x.pending_reconcile,
              "client storage is rebuilt before failed-hide rendering");

        /* Case 3: Failure in XCompositeRedirectSubwindows during restoration */
        reset_x11_stubs();
        x.unredir_fullscreen = true;
        x.fullscreen_recheck = true;
        x.fullscreen_mode = FULLSCREEN_COMPOSITED;
        x.redirected = true;
        x.overlay_hidden = false;
        x11_release_windows(&x, &e);
        tree_window_count = 1;
        tree_windows[0] = 42;
        window_x = window_y = 0;
        client_width = client_height = 1000;
        border_width = 0;
        stub_frame_has_wm_state = true;
        CHECK(x11_reconcile(&x, &e), "reconcile for bypass");
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty), "enter bypass");
        CHECK(x.fullscreen_mode == FULLSCREEN_BYPASSED, "bypassed");

        /* Injected failure in redirect during restoration */
        stub_composite_redirect_fail = true;
        client_width = 500;
        CHECK(x11_reconcile(&x, &e), "update bypassed geometry before redirect failure");
        x.fullscreen_recheck = true;
        CHECK(!x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
              "returns false when redirection reacquisition fails");

        /* Case 4: Failure in overlay reveal during restoration */
        reset_x11_stubs();
        x.unredir_fullscreen = true;
        x.fullscreen_mode = FULLSCREEN_RESTORE_HIDDEN;
        x.redirected = true;
        x.overlay_hidden = true;
        stub_shape_region_fail = true;
        CHECK(!x11_finish_fullscreen_presentation(&x, &e, &dirty),
              "returns false when overlay reveal fails");

        /* Case 5: A live window whose pixmap geometry cannot be captured must
         * keep the overlay hidden and fail before the first restoration swap. */
        reset_x11_stubs();
        x.unredir_fullscreen = true;
        x.fullscreen_recheck = true;
        x.fullscreen_mode = FULLSCREEN_COMPOSITED;
        x.redirected = true;
        x.overlay_hidden = false;
        x.root_width = x.root_height = 1000;
        x11_release_windows(&x, &e);
        tree_window_count = 1;
        tree_windows[0] = 42;
        window_x = window_y = 0;
        client_width = client_height = 1000;
        border_width = 0;
        stub_frame_has_wm_state = true;
        CHECK(x11_reconcile(&x, &e), "capture window before live-geometry failure");
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty), "enter bypass before live-geometry failure");
        client_width = 500;
        CHECK(x11_reconcile(&x, &e), "refresh bypassed geometry before restoration");
        x.fullscreen_recheck = true;
        stub_geometry_fail = true;
        CHECK(!x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
              "live pixmap geometry failure aborts restoration");
        CHECK(x.fullscreen_mode == FULLSCREEN_RESTORE_HIDDEN && x.overlay_hidden,
              "live client is never omitted from a revealed restoration scene");

        /* Case 6: QueryTree failure during restoration is not an empty scene. */
        reset_x11_stubs();
        x11_release_windows(&x, &e);
        x.unredir_fullscreen = true;
        x.fullscreen_recheck = true;
        x.fullscreen_mode = FULLSCREEN_COMPOSITED;
        x.redirected = true;
        x.overlay_hidden = false;
        x.root_width = x.root_height = 1000;
        tree_window_count = 1;
        tree_windows[0] = 42;
        window_x = window_y = 0;
        client_width = client_height = 1000;
        border_width = 0;
        stub_frame_has_wm_state = true;
        CHECK(x11_reconcile(&x, &e), "capture window before QueryTree failure");
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty), "enter bypass before QueryTree failure");
        client_width = 500;
        CHECK(x11_reconcile(&x, &e), "refresh bypassed geometry before QueryTree failure");
        x.fullscreen_recheck = true;
        stub_query_tree_fail = true;
        CHECK(!x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
              "root QueryTree failure aborts restoration");
        CHECK(x.fullscreen_mode == FULLSCREEN_RESTORE_HIDDEN && x.overlay_hidden,
              "QueryTree failure does not reveal an unverified scene");

        /* Case 7: Unexpected XCB probe errors fail strict rebuild. */
        reset_x11_stubs();
        x11_release_windows(&x, &e);
        x.unredir_fullscreen = true;
        x.fullscreen_recheck = true;
        x.fullscreen_mode = FULLSCREEN_COMPOSITED;
        x.redirected = true;
        x.overlay_hidden = false;
        x.root_width = x.root_height = 1000;
        tree_window_count = 1;
        tree_windows[0] = 42;
        window_x = window_y = 0;
        client_width = client_height = 1000;
        border_width = 0;
        stub_frame_has_wm_state = true;
        CHECK(x11_reconcile(&x, &e), "capture window before unexpected-probe failure");
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty), "enter bypass before unexpected-probe failure");
        client_width = 500;
        CHECK(x11_reconcile(&x, &e), "refresh bypassed geometry before unexpected-probe failure");
        x.fullscreen_recheck = true;
        stub_xcb_fail_geom_window = 42;
        stub_xcb_geom_error_code = BadAlloc;
        CHECK(!x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
              "unexpected XCB error aborts restoration");
        CHECK(x.fullscreen_mode == FULLSCREEN_RESTORE_HIDDEN && x.overlay_hidden,
              "unexpected probe error does not reveal an unverified scene");

        /* Case 8: An unexplained missing XCB reply also fails strict rebuild. */
        reset_x11_stubs();
        x11_release_windows(&x, &e);
        x.unredir_fullscreen = true;
        x.fullscreen_recheck = true;
        x.fullscreen_mode = FULLSCREEN_COMPOSITED;
        x.redirected = true;
        x.overlay_hidden = false;
        x.root_width = x.root_height = 1000;
        tree_window_count = 1;
        tree_windows[0] = 42;
        window_x = window_y = 0;
        client_width = client_height = 1000;
        border_width = 0;
        stub_frame_has_wm_state = true;
        CHECK(x11_reconcile(&x, &e), "capture window before missing-probe failure");
        CHECK(x11_reconcile_and_advance_fullscreen(&x, &e, &dirty), "enter bypass before missing-probe failure");
        client_width = 500;
        CHECK(x11_reconcile(&x, &e), "refresh bypassed geometry before missing-probe failure");
        x.fullscreen_recheck = true;
        stub_xcb_missing_reply_window = 42;
        CHECK(!x11_reconcile_and_advance_fullscreen(&x, &e, &dirty),
              "missing XCB reply aborts restoration");
        CHECK(x.fullscreen_mode == FULLSCREEN_RESTORE_HIDDEN && x.overlay_hidden,
              "missing probe reply does not reveal an unverified scene");
    } else {
        bool is_mixed_case = !strcmp(argv[1], "mixed-hidden-visible-damage");
        if (is_mixed_case) {
            tree_window_count = 2;
            tree_windows[0] = 42;
            tree_windows[1] = 43;
        }
        if (!strcmp(argv[1], "client-replacement")) {
            stub_frame_client = 43;
            stub_window_masks[0] = ExposureMask;
        }
        CHECK(x11_reconcile(&x, &e), "initial capture");
        CHECK(imports == (is_mixed_case ? 2 : 1), "expected initial imports");
        bool dirty = false, stop = false;
        XEvent ev = {0};
        if (!strcmp(argv[1], "client-replacement")) {
            CHECK(x.windows[0].opacity_client == 43 && x.windows[0].opacity < 0.6f,
                  "initial client opacity discovered");
            CHECK((stub_window_masks[0] & (ExposureMask | SubstructureNotifyMask)) ==
                  (ExposureMask | SubstructureNotifyMask), "discovery preserves frame subscriptions");
            stub_frame_client = 44;
            ev.type = ReparentNotify; ev.xreparent.window = 44;
            x11_handle_event(&x, &ev, &dirty, &stop);
            CHECK(x11_reconcile(&x, &e), "reconcile client replacement");
            CHECK(x.windows[0].opacity_client == 44 && x.windows[0].opacity > 0.7f &&
                  x.windows[0].opacity < 0.8f, "replace stale association and apply new opacity");
            CHECK(stub_window_masks[1] & SubstructureNotifyMask,
                  "discovery watches nested hierarchy changes");
            stub_frame_client = None;
            x.pending_changes = CHANGE_HIERARCHY;
            CHECK(x11_reconcile(&x, &e) && x.windows[0].opacity_client == None &&
                  x.windows[0].opacity == 1.0f, "removed client restores default opacity");
        } else if (!strcmp(argv[1], "remap")) {
            backing++;
            ev.type = UnmapNotify; ev.xunmap.window = 42;
            x11_handle_event(&x, &ev, &dirty, &stop);
            ev.type = MapNotify; ev.xmap.window = 42;
            x11_handle_event(&x, &ev, &dirty, &stop);
            CHECK(x11_reconcile(&x, &e), "reconcile a batched remap");
            CHECK(imports == 2 && releases == 1 && x.windows[0].pixmap == backing,
                  "remap replaces the old backing import");
        } else if (!strcmp(argv[1], "resize")) {
            client_width = 500; client_height = 300; border_width = 4; backing++;
            ev.type = ConfigureNotify; ev.xconfigure.window = 42;
            ev.xconfigure.width = client_width; ev.xconfigure.height = client_height;
            ev.xconfigure.border_width = border_width;
            x11_handle_event(&x, &ev, &dirty, &stop);
            CHECK(x11_reconcile(&x, &e), "reconcile a resize");
            CHECK(imports == 2 && releases == 1 && x.windows[0].width == 508 &&
                   x.windows[0].height == 308, "resize replaces import with correct border extents");
            x11_handle_event(&x, &ev, &dirty, &stop);
            CHECK(x11_reconcile(&x, &e) && imports == 2,
                  "duplicate configure preserves the resized import");
        } else if (!strcmp(argv[1], "reused-xid")) {
            ev.type = DestroyNotify; ev.xdestroywindow.window = 42;
            x11_handle_event(&x, &ev, &dirty, &stop);
            CHECK(x.windows[0].xid == None && !x.windows[0].viewable, "destroyed window retired immediately");
            backing++;
            CHECK(x11_reconcile(&x, &e), "reconcile recreation");
            CHECK(imports == 2 && releases == 1 && x.windows[0].xid == 42, "reused XID cleanly captured");
        } else if (!strcmp(argv[1], "retired-damage")) {
            /* Queue damage event for untracked damage handle 999 */
            ev.type = x.damage_event_base + XDamageNotify;
            XDamageNotifyEvent *dev = (XDamageNotifyEvent *)&ev;
            dev->damage = 999;
            dirty = false;
            x11_handle_event(&x, &ev, &dirty, &stop);
            CHECK(!dirty, "retired/untracked damage ID does not dirty scene");
        } else if (!strcmp(argv[1], "stale-damage-visible")) {
            x11_install_error_handler(&x);
            x.damage_opcode = 140;
            x.damage_error_base = 143;
            stub_damage_subtract_error = true;
            stub_error_damage_id = 200;

            ev.type = x.damage_event_base + XDamageNotify;
            XDamageNotifyEvent *dev = (XDamageNotifyEvent *)&ev;
            dev->damage = 200;
            dirty = false;
            x11_handle_event(&x, &ev, &dirty, &stop);

            CHECK(!dirty, "stale visible damage does not dirty scene");
            CHECK(x.windows[0].damage == None, "stale window damage retired immediately");
            CHECK(stats.live_damage == 0, "live_damage decremented on error");
            CHECK(stub_fixes_set_region_calls > 0, "scratch region reset on error");
            CHECK(x.pending_reconcile, "reconcile scheduled on stale damage error");
            CHECK(x.client_damage.count > 0,
                  "failed visible subtraction invalidates the window footprint conservatively");
            CHECK(imports == 1, "no reimport before Damage recreation");
            CHECK(x11_reconcile(&x, &e), "reconcile recreates the missing Damage resource");
            CHECK(x.windows[0].damage != None && stats.live_damage == 1,
                  "surviving window receives a replacement Damage resource");
            CHECK(imports == 1 && releases == 0,
                  "recreating Damage does not reimport the existing pixmap");
        } else if (!strcmp(argv[1], "stale-damage-occluded")) {
            x11_install_error_handler(&x);
            x.damage_opcode = 140;
            x.damage_error_base = 143;
            x.windows[0].is_occluded = true;
            stub_damage_subtract_error = true;
            stub_error_damage_id = 200;

            ev.type = x.damage_event_base + XDamageNotify;
            XDamageNotifyEvent *dev = (XDamageNotifyEvent *)&ev;
            dev->damage = 200;
            dirty = false;
            x11_handle_event(&x, &ev, &dirty, &stop);

            CHECK(!dirty, "occluded damage does not dirty scene");
            CHECK(stats.skipped_occluded_damage == 1, "skipped_occluded_damage incremented");
            CHECK(x.windows[0].damage == 200 && stats.live_damage == 1,
                  "queued BadDamage is deferred until the batch barrier");
            x11_finish_damage_batch(&x);
            CHECK(x.windows[0].damage == None, "stale window damage retired immediately");
            CHECK(stats.live_damage == 0, "live_damage decremented on error");
        } else if (!strcmp(argv[1], "mixed-hidden-visible-damage")) {
            x11_install_error_handler(&x);
            x.damage_opcode = 140;
            x.damage_error_base = 143;
            x.windows[0].is_occluded = true;
            x.windows[1].is_occluded = false;

            /* Queue a delayed hidden error for window 42 (damage 200) */
            stub_damage_subtract_error = true;
            stub_error_damage_id = 200;

            ev.type = x.damage_event_base + XDamageNotify;
            ((XDamageNotifyEvent *)&ev)->damage = 200;
            ((XDamageNotifyEvent *)&ev)->area = (XRectangle){0, 0, 10, 10};
            dirty = false;
            x11_handle_event(&x, &ev, &dirty, &stop);
            CHECK(!dirty, "hidden damage does not dirty scene");
            CHECK(x.damage_request_count == 1, "hidden subtract is tracked");
            CHECK(x.windows[0].damage == 200, "hidden damage association not retired before reply");

            /* In the same batch, queue a live visible update for window 43 (damage 201) */
            damage_bounds = (XRectangle){0, 0, 10, 10};
            ev.type = x.damage_event_base + XDamageNotify;
            ((XDamageNotifyEvent *)&ev)->damage = 201;
            ((XDamageNotifyEvent *)&ev)->area = (XRectangle){0, 0, 10, 10};
            x11_handle_event(&x, &ev, &dirty, &stop);
            CHECK(dirty, "visible damage dirties scene");

            /* The visible reply completes earlier requests: failed association retires */
            CHECK(x.windows[0].damage == None, "failed hidden association retired by visible reply");
            CHECK(x.windows[1].damage == 201, "visible association survives");
            CHECK(x11_repaint_pending(&x), "visible damage remains pending");
            CHECK(imports == 2 && stats.live_pixmaps == 2, "visible import survives without reimport");
            CHECK(x.damage_request_count == 0, "all batch requests completed by visible reply");

            /* Final completion adds no barrier after that reply */
            int syncs_before = stub_sync_calls;
            x11_finish_damage_batch(&x);
            CHECK(stub_sync_calls == syncs_before, "final completion adds no barrier after reply");

            /* Also cover a hidden request after the visible reply needing one final barrier */
            x.windows[1].is_occluded = true;
            ev.type = x.damage_event_base + XDamageNotify;
            ((XDamageNotifyEvent *)&ev)->damage = 201;
            ((XDamageNotifyEvent *)&ev)->area = (XRectangle){0, 0, 10, 10};
            x11_handle_event(&x, &ev, &dirty, &stop);
            CHECK(x.damage_request_count == 1, "trailing hidden request tracked");

            syncs_before = stub_sync_calls;
            x11_finish_damage_batch(&x);
            CHECK(stub_sync_calls == syncs_before + 1, "hidden request after reply requires final barrier");
            CHECK(x.damage_request_count == 0, "trailing request completed after barrier");
        } else if (!strcmp(argv[1], "damage-error-matching")) {
            x11_install_error_handler(&x);
            x.damage_opcode = 140;
            x.damage_error_base = 143;
            x.windows[0].is_occluded = true;
            ev.type = x.damage_event_base + XDamageNotify;
            ((XDamageNotifyEvent *)&ev)->damage = 200;
            x11_handle_event(&x, &ev, &dirty, &stop);
            CHECK(x.damage_request_count == 1 && x.windows[0].damage == 200,
                  "hidden subtract remains tracked until its batch barrier");
            _XPrivDisplay other_display = calloc(1, sizeof(*other_display));
            CHECK(other_display != NULL, "allocate unrelated display");
            XErrorEvent err = { .display = (Display *)other_display,
                .serial = stub_last_damage_subtract_serial, .error_code = 143,
                .request_code = 140, .minor_code = 3, .resourceid = 200 };
            stub_emit_x_error_event(&err);
            err.display = x.display; err.serial++;
            stub_emit_x_error_event(&err);
            err.serial = stub_last_damage_subtract_serial; err.resourceid++;
            stub_emit_x_error_event(&err);
            CHECK(!x.damage_requests[0].failed && x.windows[0].damage == 200,
                  "wrong display, serial, or resource does not suppress unrelated errors");
            err.resourceid = 200;
            stub_emit_x_error_event(&err);
            CHECK(x.damage_requests[0].failed && x.windows[0].damage == 200 &&
                  stats.live_damage == 1 && !x.pending_reconcile,
                  "matching callback records failure without mutating window state");
            x11_finish_damage_batch(&x);
            CHECK(x.windows[0].damage == None && stats.live_damage == 0 && x.pending_reconcile,
                  "failure is retired after the synchronization barrier");
            free(other_display);
        } else if (!strcmp(argv[1], "counter-accuracy")) {
            CHECK(stats.skipped_occluded_damage == 0, "0 skipped occluded initially");
            CHECK(stats.skipped_presentations == 0, "0 skipped presentations initially");
            CHECK(stats.poll_wakeups == 0, "0 poll wakeups initially");
            CHECK(stats.full_repaints == 0, "0 full repaints initially");
            CHECK(stats.partial_repaints == 0, "0 partial repaints initially");

            x.windows[0].is_occluded = true;
            ev.type = x.damage_event_base + XDamageNotify;
            XDamageNotifyEvent *dev = (XDamageNotifyEvent *)&ev;
            dev->damage = 200;
            dirty = false;
            x11_handle_event(&x, &ev, &dirty, &stop);
            CHECK(stats.skipped_occluded_damage == 1, "skipped_occluded_damage is 1");
            CHECK(!dirty, "dirty remains false for occluded damage");
        } else if (!strcmp(argv[1], "counter-deltas")) {
            CHECK(stats.imports == 1, "initial import recorded in stats");
            CHECK(stats.reimports == 0, "no reimports initially");
            CHECK(stats.live_pixmaps == 1 && stats.live_damage == 1, "live resources active");
            CHECK(stats.live_images == 1 && stats.live_textures == 1, "live EGL resources active");

            /* Event tracking */
            ev.type = x.damage_event_base + XDamageNotify;
            XDamageNotifyEvent *dev = (XDamageNotifyEvent *)&ev;
            dev->damage = 200;
            dirty = false;
            x11_handle_event(&x, &ev, &dirty, &stop);
            CHECK(stats.x_events == 1, "x_events incremented");
            CHECK(stats.damage_events == 1, "damage_events incremented");
            CHECK(dirty, "tracked damage dirtied scene");

            /* Storage reimport */
            backing++;
            ev.type = ConfigureNotify; ev.xconfigure.window = 42;
            client_width = 400; client_height = 300; border_width = 2;
            ev.xconfigure.width = 400; ev.xconfigure.height = 300; ev.xconfigure.border_width = 2;
            x11_handle_event(&x, &ev, &dirty, &stop);
            CHECK(x11_reconcile(&x, &e), "reconcile reimport");
            CHECK(stats.reimports == 1, "reimport recorded in stats");
            CHECK(stats.live_pixmaps == 1, "live_pixmaps remains 1");
        } else {
            CHECK(!strcmp(argv[1], "bordered-move"), "known regression case");
            window_x = 20; window_y = 30;
            ev.type = ConfigureNotify; ev.xconfigure.window = 42;
            ev.xconfigure.x = 20; ev.xconfigure.y = 30;
            ev.xconfigure.width = 320; ev.xconfigure.height = 240; ev.xconfigure.border_width = 2;
            x11_handle_event(&x, &ev, &dirty, &stop);
            CHECK(x11_reconcile(&x, &e), "reconcile a bordered move");
            CHECK(imports == 1 && releases == 0, "move preserves the bordered window import");
            CHECK(x.windows[0].x == 20 && x.windows[0].y == 30, "move uses current server geometry");
        }
    }
    CHECK(stub_xcb_outstanding == 0 && stub_xcb_invalid_replies == 0,
          "all queries consumed exactly once without invalid cookie retrievals");
    x11_release_windows(&x, &e);
    CHECK(stats.live_pixmaps == 0 && stats.live_images == 0 &&
          stats.live_textures == 0 && stats.live_damage == 0 &&
          stats.live_regions == 0,
          "all live resources return to 0 at release");
    int released = releases;
    x11_release_windows(&x, &e);
    CHECK(releases == released && x.num_windows == 0 && x.windows == NULL,
          "window cleanup is idempotent");
    CHECK(stats.live_pixmaps == 0 && stats.live_images == 0 &&
          stats.live_textures == 0 && stats.live_damage == 0 &&
          stats.live_regions == 0,
          "all live resources remain 0 on repeated release");
    free(display);
    printf("PASS: %s\n", argv[1]);
    return 0;
}
