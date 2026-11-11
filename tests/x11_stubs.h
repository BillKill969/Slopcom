#ifndef TEST_X11_STUBS_H
#define TEST_X11_STUBS_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <X11/Xlib-xcb.h>
#include <xcb/xcb.h>
#include "x11.h"
#include "x11_errors.h"
#include "stats.h"
#include "background.h"

typedef enum {
    STUB_XCB_OP_ATTR_SEND,
    STUB_XCB_OP_GEOM_SEND,
    STUB_XCB_OP_FLUSH,
    STUB_XCB_OP_ATTR_RECV,
    STUB_XCB_OP_GEOM_RECV,
} StubXCBOpKind;

typedef struct {
    StubXCBOpKind op;
    Window window;
    unsigned int sequence;
} StubXCBLogEntry;

#define MAX_STUB_XCB_OPS 128
extern StubXCBLogEntry stub_xcb_ops[MAX_STUB_XCB_OPS];
extern int stub_xcb_op_count;
extern int stub_xcb_send_count;
extern int stub_xcb_recv_count;
extern int stub_xcb_outstanding;
extern int stub_xcb_invalid_replies;
extern int stub_xcb_flush_count;
extern int stub_xflush_calls;
extern int stub_serial_attr_calls;

extern bool stub_xcb_connection_error;
extern bool stub_xcb_flush_failure;
extern int stub_xcb_disconnect_after_replies;
extern Window stub_xcb_fail_attr_window;
extern Window stub_xcb_fail_geom_window;
extern unsigned char stub_xcb_attr_error_code;
extern unsigned char stub_xcb_geom_error_code;
extern Window stub_xcb_missing_reply_window;
extern Window stub_xcb_input_only_window;
extern Window stub_xcb_unmapped_window;
extern Window stub_xcb_override_redirect_window;
extern VisualID stub_xcb_window_visual_id[16];
extern int stub_xcb_window_x[16];
extern int stub_xcb_window_y[16];
extern int stub_xcb_window_w[16];
extern int stub_xcb_window_h[16];
extern int stub_xcb_window_bw[16];
extern bool stub_xcb_has_custom_geom[16];

typedef struct {
    Atom type;
    int format;
    unsigned long count;
    unsigned long remaining;
    unsigned long value;
    bool is_set;
} StubProp;

extern int imports, releases;
extern bool import_ok, exists, vanish_on_import, fail_on_window_43;
extern Pixmap backing;
extern int client_width, client_height, border_width, window_x, window_y;
extern int tree_window_count;
extern Window tree_windows[8];
extern bool empty_shape, translucent_property;
extern XRectangle damage_bounds;
extern XserverRegion captured_damage;
extern bool stub_fetch_region_alloc_fail;
extern bool stub_fetch_region_empty;

extern int free_pixmap_calls;
extern StubProp stub_primary_prop;
extern StubProp stub_fallback_prop;
extern StubProp stub_hint_prop;
extern bool stub_geometry_fail;
extern bool stub_query_tree_fail;
extern bool stub_geometry_override;
extern Window stub_geometry_root;
extern unsigned int stub_geometry_width;
extern unsigned int stub_geometry_height;
extern unsigned int stub_geometry_depth;
extern Pixmap stub_fail_import_pixmap;
extern Window stub_frame_client;
extern bool stub_frame_has_wm_state;
extern bool stub_client_override_redirect;
extern bool stub_client_shaped;
extern VisualID stub_client_visual_id;
extern bool stub_client_has_custom_geom;
extern int stub_client_w, stub_client_h, stub_client_border_width;
extern bool stub_bypass_hint_set;
extern unsigned long stub_bypass_hint_value;
extern bool stub_bypass_hint_malformed;
extern bool stub_opacity_set;
extern unsigned long stub_opacity_value;
extern bool stub_opacity_malformed;
extern int stub_composite_redirect_calls;
extern int stub_composite_unredirect_calls;
extern bool stub_composite_redirect_fail;
extern bool stub_composite_unredirect_fail;
extern bool stub_root_redirected;
extern int stub_shape_set_bounding_calls;
extern int stub_shape_set_input_calls;
extern unsigned long stub_shape_masks[3];
extern bool stub_shape_region_fail;
extern XserverRegion stub_last_bounding_shape_region;
extern int stub_translate_coords_calls;
extern bool stub_translate_coords_fail;
extern int stub_translate_dest_x;
extern int stub_translate_dest_y;
extern long stub_window_masks[3];
extern bool stub_damage_subtract_error;
extern Damage stub_error_damage_id;
extern unsigned long stub_last_damage_subtract_serial;
extern bool stub_damage_create_error;
extern unsigned char stub_damage_create_error_code;
extern int stub_fixes_set_region_calls;
extern int stub_sync_calls;
extern int stub_pending_events;
void stub_emit_x_error(Display *display, unsigned char code);
void stub_emit_x_error_event(const XErrorEvent *event);

void reset_x11_stubs(void);

#endif
