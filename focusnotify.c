#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    int x;
    int y;
    int w;
    int h;
} Geometry;

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

static void send_notification(int w, int h) {
    char message[128];
    snprintf(message, sizeof(message), "Width: %d, Height: %d", w, h);

    pid_t pid = fork();
    if (pid == 0) {
        execlp("notify-send", "notify-send", "Focused window size", message, (char *)NULL);
        _exit(127);
    }

    if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

static void notify_active_window_size(Display *dpy, Window root, Window *last_win) {
    Window active = None;
    if (!get_active_window(dpy, root, &active)) {
        return;
    }

    if (active == *last_win) {
        return;
    }

    Geometry g;
    if (!get_window_geometry(dpy, active, &g)) {
        return;
    }

    send_notification(g.w, g.h);
    *last_win = active;
}

int main(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "focusnotify: could not open X display\n");
        return 1;
    }

    Window root = DefaultRootWindow(dpy);
    Atom active_atom = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);

    XSelectInput(dpy, root, PropertyChangeMask);

    Window last_win = None;
    notify_active_window_size(dpy, root, &last_win);

    while (1) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        if (ev.type == PropertyNotify && ev.xproperty.window == root && ev.xproperty.atom == active_atom) {
            notify_active_window_size(dpy, root, &last_win);
        }
    }

    XCloseDisplay(dpy);
    return 0;
}
