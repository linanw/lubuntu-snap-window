#define _POSIX_C_SOURCE 200809L

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

static bool is_qt_window(Display *dpy, Window win) {
    XClassHint hint;
    bool is_match = false;

    hint.res_name = NULL;
    hint.res_class = NULL;
    if (!XGetClassHint(dpy, win, &hint)) {
        return false;
    }

    if (hint.res_class && (strstr(hint.res_class, "Qt") != NULL || strstr(hint.res_class, "qt") != NULL)) {
        is_match = true;
    }

    if (hint.res_name) {
        XFree(hint.res_name);
    }
    if (hint.res_class) {
        XFree(hint.res_class);
    }

    return is_match;
}

static void move_resize_window_outer(Display *dpy, Window win, int x, int y, int w, int h) {
    FrameExtents extents;
    if (get_frame_extents(dpy, win, &extents)) {
        if (is_qt_window(dpy, win)) {
            x += extents.left;
            y += extents.top;
        }
        w -= extents.left + extents.right;
        h -= extents.top + extents.bottom;
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

static void snap_window_to_side(Display *dpy, Window win, int side, int sw, int sh) {
    Window root = DefaultRootWindow(dpy);
    int work_x = 0;
    int work_y = 0;
    int work_w = sw;
    int work_h = sh;
    int x = 0;
    int y = 0;
    int w = (work_w / 2) - 2;
    int h = work_h - 2;

    if (get_workarea(dpy, root, &work_x, &work_y, &work_w, &work_h)) {
        y = work_y;
        w = (work_w / 2) - 2;
        h = work_h - 2;
        x = (side == 0) ? work_x : (work_x + (work_w / 2));
    } else {
        y = 0;
        x = (side == 0) ? 0 : (sw / 2);
    }

    move_resize_window_outer(dpy, win, x, y, w, h);
}

static void snap_window_to_top(Display *dpy, Window win, int sw, int sh) {
    Window root = DefaultRootWindow(dpy);
    int work_x = 0;
    int work_y = 0;
    int work_w = sw;
    int work_h = sh;
    int x = 0;
    int y = 0;
    int w = sw - 2;
    int h = sh - 2;

    if (get_workarea(dpy, root, &work_x, &work_y, &work_w, &work_h)) {
        x = work_x;
        y = work_y;
        w = work_w - 2;
        h = work_h - 2;
    }

    move_resize_window_outer(dpy, win, x, y, w, h);
}

static void snap_window_to_corner(Display *dpy, Window win, int corner, int sw, int sh) {
    Window root = DefaultRootWindow(dpy);
    int half_w = sw / 2;
    int half_h = sh / 2;
    int x = 0;
    int y = 0;
    int w = half_w - 2;
    int h = half_h - 2;
    int work_x = 0;
    int work_y = 0;
    int work_w = sw;
    int work_h = sh;
    bool have_workarea = get_workarea(dpy, root, &work_x, &work_y, &work_w, &work_h);

    switch (corner) {
        case 0:
            // Top-left: 30% width and full usable work area height.
            x = have_workarea ? work_x : 0;
            y = have_workarea ? work_y : 0;
            w = (int)(work_w * 0.30) - 2;
            h = work_h - 2;
            break;
        case 1:
            // Top-right: 70% width and full usable work area height.
            x = have_workarea ? (work_x + (int)(work_w * 0.30)) : (int)(sw * 0.30);
            y = have_workarea ? work_y : 0;
            w = (int)(work_w * 0.70) - 2;
            h = work_h - 2;
            break;
        case 2:
            x = 0;
            y = half_h;
            break;
        case 3:
            x = half_w;
            y = half_h;
            break;
        default:
            return;
    }

    move_resize_window_outer(dpy, win, x, y, w, h);
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

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        log_line("ERROR", "could not open X display");
        return 1;
    }

    XSetErrorHandler(on_x_error);
    XSetIOErrorHandler(on_x_io_error);

    Window root = DefaultRootWindow(dpy);

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
                    snap_window_to_top(dpy, drag_win, sw, sh);
                } else {
                    int side = -1;
                    if (moved && in_side_edge(root_x, sw, &side)) {
                        if (verbose_logs) {
                            log_line("INFO", "snapping to side");
                        }
                        snap_window_to_side(dpy, drag_win, side, sw, sh);
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
