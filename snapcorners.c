#define _POSIX_C_SOURCE 200809L

#include "snapconfig.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CORNER_SIZE 2
#define POLL_USEC 20000
#define MOVERESIZE_FLAG_X (1L << 8)
#define MOVERESIZE_FLAG_Y (1L << 9)
#define MOVERESIZE_FLAG_W (1L << 10)
#define MOVERESIZE_FLAG_H (1L << 11)
#define MOVERESIZE_SOURCE_PAGER (2L << 12)

typedef struct {
    int x;
    int y;
    int w;
    int h;
} Geometry;

typedef struct {
    int left;
    int right;
    int top;
    int bottom;
} FrameExtents;

static volatile sig_atomic_t keep_running = 1;
static bool verbose_logs = false;

/* ── Profile state ──────────────────────────────────────────────────────── */

static SnapConfig snap_config;
static bool config_loaded = false;

/* Built-in fallback triggers (original hard-coded behaviour). */
static const SnapTriggers BUILTIN_TRIGGERS = {
    /* corner[4]: TL, TR, BL, BR */
    .corner = {
        {0.00, 0.0, 0.30, 1.0},
        {0.30, 0.0, 0.70, 1.0},
        {0.00, 0.5, 0.50, 0.5},
        {0.50, 0.5, 0.50, 0.5},
    },
    /* side[2]: left, right */
    .side = {
        {0.00, 0.0, 0.50, 1.0},
        {0.50, 0.0, 0.50, 1.0},
    },
    .top_edge    = {0.00, 0.0, 1.00, 1.0},
    .bottom_edge = {0.00, 0.0, 1.00, 1.0},
    .n_top_zones = 0,
    .n_bottom_zones = 0,
    .n_left_zones = 0,
    .n_right_zones = 0,
    .valid       = 1,
};

static const SnapTriggers *active_triggers = &BUILTIN_TRIGGERS;

static void log_line(const char *level, const char *msg) {
    time_t now = time(NULL);
    struct tm tm_now;
    char ts[32];

    if (localtime_r(&now, &tm_now) == NULL) {
        fprintf(stderr, "snapcorners [%s] %s\n", level, msg);
        return;
    }

    if (strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now) == 0) {
        fprintf(stderr, "snapcorners [%s] %s\n", level, msg);
        return;
    }

    fprintf(stderr, "snapcorners %s [%s] %s\n", ts, level, msg);
}

static void on_signal(int signo) {
    (void)signo;
    keep_running = 0;
}

static int on_x_error(Display *dpy, XErrorEvent *event) {
    char errtxt[256];
    char msg[512];
    (void)dpy;

    if (event->error_code == BadWindow) {
        if (verbose_logs) {
            XGetErrorText(dpy, event->error_code, errtxt, (int)sizeof(errtxt));
            snprintf(
                msg,
                sizeof(msg),
                "XError code=%d (%s), request=%d, minor=%d, resource=0x%lx",
                event->error_code,
                errtxt,
                event->request_code,
                event->minor_code,
                event->resourceid);
            log_line("DEBUG", msg);
        }
        return 0;
    }

    XGetErrorText(dpy, event->error_code, errtxt, (int)sizeof(errtxt));
    snprintf(
        msg,
        sizeof(msg),
        "XError code=%d (%s), request=%d, minor=%d, resource=0x%lx",
        event->error_code,
        errtxt,
        event->request_code,
        event->minor_code,
        event->resourceid);
    log_line("WARN", msg);
    return 0;
}

static int on_x_io_error(Display *dpy) {
    (void)dpy;
    log_line("ERROR", "X I/O error: X server connection was lost");
    return 0;
}

static bool get_active_window(Display *dpy, Window root, Window *out_win) {
    Atom prop = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        dpy,
        root,
        prop,
        0,
        1,
        False,
        AnyPropertyType,
        &actual_type,
        &actual_format,
        &nitems,
        &bytes_after,
        &data);

    if (status != Success || !data || nitems < 1 || actual_format != 32) {
        if (data) {
            XFree(data);
        }
        return false;
    }

    *out_win = *(Window *)data;
    XFree(data);

    return *out_win != None;
}

static bool get_window_geometry(Display *dpy, Window win, Geometry *geom) {
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(dpy, win, &attrs) || attrs.map_state != IsViewable) {
        return false;
    }

    Window child;
    int abs_x = 0;
    int abs_y = 0;

    if (!XTranslateCoordinates(dpy, win, DefaultRootWindow(dpy), 0, 0, &abs_x, &abs_y, &child)) {
        return false;
    }

    geom->x = abs_x;
    geom->y = abs_y;
    geom->w = attrs.width;
    geom->h = attrs.height;

    return true;
}

static bool get_outer_window(Display *dpy, Window root, Window win, Window *out_win) {
    Window current = win;

    while (current != None && current != root) {
        Window root_ret = None;
        Window parent_ret = None;
        Window *children = NULL;
        unsigned int child_count = 0;

        if (!XQueryTree(dpy, current, &root_ret, &parent_ret, &children, &child_count)) {
            return false;
        }

        if (children) {
            XFree(children);
        }

        if (parent_ret == None) {
            break;
        }

        if (parent_ret == root) {
            *out_win = current;
            return true;
        }

        current = parent_ret;
    }

    *out_win = win;
    return true;
}

static bool get_outer_window_geometry(Display *dpy, Window root, Window win, Geometry *geom) {
    Window outer = None;
    if (!get_outer_window(dpy, root, win, &outer)) {
        return false;
    }

    return get_window_geometry(dpy, outer, geom);
}

static bool wm_supports_atom(Display *dpy, Window root, const char *atom_name) {
    Atom supported_atom = XInternAtom(dpy, "_NET_SUPPORTED", False);
    Atom needle = XInternAtom(dpy, atom_name, False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        dpy,
        root,
        supported_atom,
        0,
        1024,
        False,
        XA_ATOM,
        &actual_type,
        &actual_format,
        &nitems,
        &bytes_after,
        &data);

    if (status != Success || !data || actual_type != XA_ATOM || actual_format != 32) {
        if (data) {
            XFree(data);
        }
        return false;
    }

    bool found = false;
    Atom *atoms = (Atom *)data;
    for (unsigned long i = 0; i < nitems; ++i) {
        if (atoms[i] == needle) {
            found = true;
            break;
        }
    }

    XFree(data);
    return found;
}

static bool is_snappable_window(Display *dpy, Window win) {
    XWindowAttributes attrs;
    if (win == None || !XGetWindowAttributes(dpy, win, &attrs)) {
        return false;
    }
    return attrs.map_state == IsViewable;
}

static bool get_workarea(Display *dpy, Window root, int *x, int *y, int *w, int *h) {
    Atom prop = XInternAtom(dpy, "_NET_WORKAREA", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        dpy,
        root,
        prop,
        0,
        4,
        False,
        AnyPropertyType,
        &actual_type,
        &actual_format,
        &nitems,
        &bytes_after,
        &data);

    if (status != Success || !data || nitems < 4 || actual_format != 32) {
        if (data) {
            XFree(data);
        }
        return false;
    }

    long *vals = (long *)data;
    *x = (int)vals[0];
    *y = (int)vals[1];
    *w = (int)vals[2];
    *h = (int)vals[3];

    XFree(data);
    return *w > 0 && *h > 0;
}

static bool get_frame_extents(Display *dpy, Window win, FrameExtents *extents) {
    Atom prop = XInternAtom(dpy, "_NET_FRAME_EXTENTS", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    extents->left = 0;
    extents->right = 0;
    extents->top = 0;
    extents->bottom = 0;

    int status = XGetWindowProperty(
        dpy,
        win,
        prop,
        0,
        4,
        False,
        XA_CARDINAL,
        &actual_type,
        &actual_format,
        &nitems,
        &bytes_after,
        &data);

    if (status != Success || !data || actual_type != XA_CARDINAL || actual_format != 32 || nitems < 4) {
        if (data) {
            XFree(data);
        }
        return false;
    }

    long *vals = (long *)data;
    extents->left = (int)vals[0];
    extents->right = (int)vals[1];
    extents->top = (int)vals[2];
    extents->bottom = (int)vals[3];

    XFree(data);
    return true;
}

static bool move_resize_window_outer_via_wm(Display *dpy, Window win, int x, int y, int w, int h) {
    Window root = DefaultRootWindow(dpy);
    Geometry client_geom;
    Geometry outer_geom;
    int client_w = w;
    int client_h = h;
    XEvent ev;

    if (!wm_supports_atom(dpy, root, "_NET_MOVERESIZE_WINDOW")) {
        return false;
    }

    if (get_window_geometry(dpy, win, &client_geom) &&
        get_outer_window_geometry(dpy, root, win, &outer_geom)) {
        client_w -= outer_geom.w - client_geom.w;
        client_h -= outer_geom.h - client_geom.h;
    } else {
        FrameExtents extents;
        if (get_frame_extents(dpy, win, &extents)) {
            client_w -= extents.left + extents.right;
            client_h -= extents.top + extents.bottom;
        }
    }

    if (client_w < 50) {
        client_w = 50;
    }
    if (client_h < 50) {
        client_h = 50;
    }

    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = XInternAtom(dpy, "_NET_MOVERESIZE_WINDOW", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = NorthWestGravity |
                           MOVERESIZE_FLAG_X |
                           MOVERESIZE_FLAG_Y |
                           MOVERESIZE_FLAG_W |
                           MOVERESIZE_FLAG_H |
                           MOVERESIZE_SOURCE_PAGER;
    ev.xclient.data.l[1] = x;
    ev.xclient.data.l[2] = y;
    ev.xclient.data.l[3] = client_w;
    ev.xclient.data.l[4] = client_h;

    if (XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev) == 0) {
        return false;
    }

    XFlush(dpy);
    return true;
}

static void move_resize_window_outer(Display *dpy, Window win, int x, int y, int w, int h) {
    Window root = DefaultRootWindow(dpy);
    Geometry client_geom;
    Geometry outer_geom;

    if (move_resize_window_outer_via_wm(dpy, win, x, y, w, h)) {
        return;
    }

    if (get_window_geometry(dpy, win, &client_geom) && get_outer_window_geometry(dpy, root, win, &outer_geom)) {
        x += client_geom.x - outer_geom.x;
        y += client_geom.y - outer_geom.y;
        w -= outer_geom.w - client_geom.w;
        h -= outer_geom.h - client_geom.h;
    } else {
        FrameExtents extents;
        if (get_frame_extents(dpy, win, &extents)) {
            w -= extents.left + extents.right;
            h -= extents.top + extents.bottom;
        }
    }

    if (w < 50) {
        w = 50;
    }
    if (h < 50) {
        h = 50;
    }

    XMoveResizeWindow(dpy, win, x, y, (unsigned int)w, (unsigned int)h);
    XFlush(dpy);
}

static bool in_corner(int px, int py, int sw, int sh, int *corner) {
    if (px <= CORNER_SIZE && py <= CORNER_SIZE) {
        *corner = 0; // top-left
        return true;
    }
    if (px >= sw - CORNER_SIZE && py <= CORNER_SIZE) {
        *corner = 1; // top-right
        return true;
    }
    if (px <= CORNER_SIZE && py >= sh - CORNER_SIZE) {
        *corner = 2; // bottom-left
        return true;
    }
    if (px >= sw - CORNER_SIZE && py >= sh - CORNER_SIZE) {
        *corner = 3; // bottom-right
        return true;
    }
    return false;
}

static bool in_side_edge(int px, int sw, int *side) {
    if (px <= CORNER_SIZE) {
        *side = 0; // left
        return true;
    }
    if (px >= sw - CORNER_SIZE) {
        *side = 1; // right
        return true;
    }
    return false;
}

static bool in_top_edge(int py) {
    return py <= CORNER_SIZE;
}

static bool in_bottom_edge(int py, int sh) {
    return py >= sh - CORNER_SIZE;
}

static bool window_is_dock(Display *dpy, Window win, Atom wm_type_atom, Atom dock_atom) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        dpy,
        win,
        wm_type_atom,
        0,
        8,
        False,
        XA_ATOM,
        &actual_type,
        &actual_format,
        &nitems,
        &bytes_after,
        &data);

    if (status != Success || !data || actual_type != XA_ATOM || actual_format != 32) {
        if (data) {
            XFree(data);
        }
        return false;
    }

    bool is_dock = false;
    Atom *atoms = (Atom *)data;
    for (unsigned long i = 0; i < nitems; ++i) {
        if (atoms[i] == dock_atom) {
            is_dock = true;
            break;
        }
    }

    XFree(data);
    return is_dock;
}

static int resize_horizontal_docks_to_screen_width(Display *dpy, Window root, int sw) {
    Atom wm_type_atom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom dock_atom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    Window root_ret = None;
    Window parent_ret = None;
    Window *children = NULL;
    unsigned int child_count = 0;
    int resized = 0;

    if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &child_count)) {
        return 0;
    }

    for (unsigned int i = 0; i < child_count; ++i) {
        Window win = children[i];
        XWindowAttributes attrs;
        if (!XGetWindowAttributes(dpy, win, &attrs) || attrs.map_state != IsViewable) {
            continue;
        }

        if (!window_is_dock(dpy, win, wm_type_atom, dock_atom)) {
            continue;
        }

        // Only resize horizontal docks/panels; leave vertical docks untouched.
        if (attrs.width < attrs.height) {
            continue;
        }

        if (attrs.x == 0 && attrs.width == sw) {
            continue;
        }

        XMoveResizeWindow(dpy, win, 0, attrs.y, (unsigned int)sw, (unsigned int)attrs.height);
        resized++;
    }

    if (children) {
        XFree(children);
    }
    if (resized > 0) {
        XFlush(dpy);
    }
    return resized;
}

/*
 * Apply a SnapTarget relative to the given work area.
 * x/y are offsets from work_x/work_y; w/h are fractions of work_w/work_h.
 */
static void snap_window_to_target(Display *dpy, Window win,
                                   const SnapTarget *t,
                                   int work_x, int work_y,
                                   int work_w, int work_h) {
    int x = work_x + (int)(t->x_frac * work_w);
    int y = work_y + (int)(t->y_frac * work_h);
    int w = (int)(t->w_frac * work_w) - 2;
    int h = (int)(t->h_frac * work_h) - 2;
    if (w < 50) w = 50;
    if (h < 50) h = 50;
    move_resize_window_outer(dpy, win, x, y, w, h);
}

static void get_workarea_or_screen(Display *dpy, int sw, int sh,
                                    int *wx, int *wy, int *ww, int *wh) {
    Window root = DefaultRootWindow(dpy);
    *wx = 0; *wy = 0; *ww = sw; *wh = sh;
    get_workarea(dpy, root, wx, wy, ww, wh);
}

static void snap_window_to_side(Display *dpy, Window win, int side,
                                 int sw, int sh, int mouse_y) {
    int wx, wy, ww, wh;
    get_workarea_or_screen(dpy, sw, sh, &wx, &wy, &ww, &wh);

    const SideEdgeZone *zones = (side == 0) ? active_triggers->left_zones : active_triggers->right_zones;
    int zone_count = (side == 0) ? active_triggers->n_left_zones : active_triggers->n_right_zones;
    double my_frac = (sh > 0) ? ((double)mouse_y / sh) : 0.5;

    for (int i = 0; i < zone_count; i++) {
        const SideEdgeZone *z = &zones[i];
        if (my_frac >= z->mouse_y_min_frac && my_frac <= z->mouse_y_max_frac) {
            snap_window_to_target(dpy, win, &z->target, wx, wy, ww, wh);
            return;
        }
    }

    snap_window_to_target(dpy, win, &active_triggers->side[side],
                          wx, wy, ww, wh);
}

static void snap_window_to_top(Display *dpy, Window win,
                                int sw, int sh, int mouse_x) {
    int wx, wy, ww, wh;
    get_workarea_or_screen(dpy, sw, sh, &wx, &wy, &ww, &wh);

    /* Check top-edge sub-zones first (matched by mouse x fraction). */
    double mx_frac = (sw > 0) ? ((double)mouse_x / sw) : 0.5;
    for (int i = 0; i < active_triggers->n_top_zones; i++) {
        const TopEdgeZone *z = &active_triggers->top_zones[i];
        if (mx_frac >= z->mouse_x_min_frac && mx_frac <= z->mouse_x_max_frac) {
            snap_window_to_target(dpy, win, &z->target, wx, wy, ww, wh);
            return;
        }
    }

    snap_window_to_target(dpy, win, &active_triggers->top_edge,
                          wx, wy, ww, wh);
}

static void snap_window_to_bottom(Display *dpy, Window win,
                                   int sw, int sh, int mouse_x) {
    int wx, wy, ww, wh;
    get_workarea_or_screen(dpy, sw, sh, &wx, &wy, &ww, &wh);

    double mx_frac = (sw > 0) ? ((double)mouse_x / sw) : 0.5;
    for (int i = 0; i < active_triggers->n_bottom_zones; i++) {
        const TopEdgeZone *z = &active_triggers->bottom_zones[i];
        if (mx_frac >= z->mouse_x_min_frac && mx_frac <= z->mouse_x_max_frac) {
            snap_window_to_target(dpy, win, &z->target, wx, wy, ww, wh);
            return;
        }
    }

    if (active_triggers->n_bottom_zones > 0) {
        snap_window_to_target(dpy, win, &active_triggers->bottom_edge,
                              wx, wy, ww, wh);
    }
}

static void snap_window_to_corner(Display *dpy, Window win, int corner,
                                   int sw, int sh) {
    if (corner < 0 || corner > 3) return;
    int wx, wy, ww, wh;
    get_workarea_or_screen(dpy, sw, sh, &wx, &wy, &ww, &wh);
    snap_window_to_target(dpy, win, &active_triggers->corner[corner],
                          wx, wy, ww, wh);
}

static void sleep_poll_interval(void) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = POLL_USEC * 1000L;
    nanosleep(&ts, NULL);
}

int main(void) {
    const char *verbose_env = getenv("SNAPCORNERS_VERBOSE");
    if (verbose_env && strcmp(verbose_env, "1") == 0) {
        verbose_logs = true;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Load profile config (non-fatal if missing). */
    const char *cfg_path = get_config_path();
    if (cfg_path) {
        config_loaded = (load_snap_config(cfg_path, &snap_config) != 0);
        if (config_loaded) {
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "loaded %d profile(s) from %s",
                     snap_config.n_profiles, cfg_path);
            log_line("INFO", msg);
        } else if (verbose_logs) {
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "no profile config at %s, using built-in defaults",
                     cfg_path);
            log_line("INFO", msg);
        }
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        log_line("ERROR", "could not open X display");
        return 1;
    }

    XSetErrorHandler(on_x_error);
    XSetIOErrorHandler(on_x_io_error);

    Window root = DefaultRootWindow(dpy);

    /* Select the initial profile based on current screen width. */
    if (config_loaded) {
        int init_sw = DisplayWidth(dpy, DefaultScreen(dpy));
        const SnapProfile *prof = select_profile(&snap_config, init_sw);
        if (prof) {
            active_triggers = &prof->triggers;
            if (verbose_logs) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "activated profile \"%s\" for width %d",
                         prof->name, init_sw);
                log_line("INFO", msg);
            }
        }
    }

    bool was_left_down = false;
    bool drag_candidate = false;
    Geometry drag_start = {0, 0, 0, 0};
    Window drag_win = None;
    int prev_sw = -1;
    int prev_sh = -1;

    if (verbose_logs) {
        log_line("INFO", "started");
    }

    while (keep_running) {
        Window root_ret, child_ret;
        int root_x = 0, root_y = 0, win_x = 0, win_y = 0;
        unsigned int mask = 0;

        if (!XQueryPointer(dpy, root, &root_ret, &child_ret, &root_x, &root_y, &win_x, &win_y, &mask)) {
            if (verbose_logs) {
                log_line("WARN", "XQueryPointer returned false");
            }
            sleep_poll_interval();
            continue;
        }

        bool left_down = (mask & Button1Mask) != 0;
        int sw = DisplayWidth(dpy, DefaultScreen(dpy));
        int sh = DisplayHeight(dpy, DefaultScreen(dpy));

        if (sw != prev_sw || sh != prev_sh) {
            int resized = resize_horizontal_docks_to_screen_width(dpy, root, sw);
            if (verbose_logs) {
                char msg[256];
                snprintf(
                    msg,
                    sizeof(msg),
                    "resolution changed: %dx%d -> %dx%d, resized %d dock window(s)",
                    prev_sw,
                    prev_sh,
                    sw,
                    sh,
                    resized);
                log_line("INFO", msg);
            }
            prev_sw = sw;
            prev_sh = sh;

            /* Select the matching snap profile for the new resolution. */
            if (config_loaded) {
                const SnapProfile *prof = select_profile(&snap_config, sw);
                if (prof) {
                    active_triggers = &prof->triggers;
                    if (verbose_logs) {
                        char msg[128];
                        snprintf(msg, sizeof(msg),
                                 "activated profile \"%s\" for width %d",
                                 prof->name, sw);
                        log_line("INFO", msg);
                    }
                } else {
                    active_triggers = &BUILTIN_TRIGGERS;
                    if (verbose_logs)
                        log_line("INFO", "no matching profile, using built-in defaults");
                }
            }
        }

        if (left_down && !was_left_down) {
            Window active = None;
            Geometry g;

            if (get_active_window(dpy, root, &active) &&
                is_snappable_window(dpy, active) &&
                get_window_geometry(dpy, active, &g)) {
                drag_candidate = true;
                drag_start = g;
                drag_win = active;
            } else {
                drag_candidate = false;
                drag_win = None;
            }
        }

        if (!left_down && was_left_down && drag_candidate && drag_win != None) {
            Geometry end;
            bool moved = false;
            if (get_window_geometry(dpy, drag_win, &end)) {
                moved = (end.x != drag_start.x) || (end.y != drag_start.y);
            }

            int corner = -1;
            if (moved && in_corner(root_x, root_y, sw, sh, &corner)) {
                if (verbose_logs) {
                    log_line("INFO", "snapping to corner");
                }
                snap_window_to_corner(dpy, drag_win, corner, sw, sh);
            } else {
                if (moved && in_top_edge(root_y)) {
                    if (verbose_logs) {
                        log_line("INFO", "snapping to top edge");
                    }
                    snap_window_to_top(dpy, drag_win, sw, sh, root_x);
                } else {
                    if (moved && in_bottom_edge(root_y, sh) && active_triggers->n_bottom_zones > 0) {
                        if (verbose_logs) {
                            log_line("INFO", "snapping to bottom edge");
                        }
                        snap_window_to_bottom(dpy, drag_win, sw, sh, root_x);
                    } else {
                        int side = -1;
                        if (moved && in_side_edge(root_x, sw, &side)) {
                            if (verbose_logs) {
                                log_line("INFO", "snapping to side");
                            }
                            snap_window_to_side(dpy, drag_win, side, sw, sh, root_y);
                        }
                    }
                }
            }

            drag_candidate = false;
            drag_win = None;
        }

        was_left_down = left_down;
        sleep_poll_interval();
    }

    if (verbose_logs) {
        log_line("INFO", "stopping");
    }
    XCloseDisplay(dpy);
    return 0;
}
