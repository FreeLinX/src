#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/select.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#define PANEL_H 28
#define MAX_TASKS 32
#define MAX_MENU_ITEMS 10

typedef struct {
    Window win;
    char title[128];
    int is_active;
    int is_minimized;
    int x, w;
} TaskItem;

typedef struct {
    const char *label;
    const char *cmd;
} MenuItem;

static MenuItem menu_items[MAX_MENU_ITEMS] = {
    {"Terminal (uxterm)",     "uxterm &"},
    {"File Manager (flx-fm)", "flx-fm &"},
    {"Web Browser",           "flx-browser &"},
    {"Process Monitor (htop)","uxterm -T 'Process Monitor' -e htop &"},
    {"Text Editor (flx-pad)", "flx-pad &"},
    {"Image Viewer (flx-view)","flx-view &"},
    {"3D Gears (glxgears)",   "glxgears &"},
    {"Screen Magnifier (xmag)","xmag &"},
    {"Classic Doom",          "doom &"},
    {"Sleep / Suspend (zzz)", "zzz &"}
};

static TaskItem tasks[MAX_TASKS];
static int task_count = 0;
static Window active_win = None;
static int menu_open = 0;
static Window menu_win = None;
static Pixmap menu_backbuffer = None;
static cairo_surface_t *menu_cs = NULL;
static cairo_t *menu_cr = NULL;
static int menu_w = 210;
static int menu_h = MAX_MENU_ITEMS * 26 + 32;

static char mem_str[32] = "";
static char clock_str[32] = "";
static char date_str[32] = "";

static Atom atom_net_client_list;
static Atom atom_net_active_window;
static Atom atom_net_wm_name;
static Atom atom_utf8_string;
static Atom atom_wm_state;
static Atom atom_net_wm_state;
static Atom atom_net_wm_window_type;
static Atom atom_net_wm_window_type_dock;
static Atom atom_net_wm_window_type_desktop;

static void update_sysinfo(void) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) {
        strftime(clock_str, sizeof(clock_str), "%H:%M:%S", tm);
        strftime(date_str, sizeof(date_str), "%b %d", tm);
    }

    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        unsigned long total = 0, avail = 0;
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "MemTotal: %lu kB", &total) == 1) {}
            else if (sscanf(line, "MemAvailable: %lu kB", &avail) == 1) {}
        }
        fclose(f);
        if (total > 0) {
            unsigned long used_mb = (total - avail) / 1024;
            snprintf(mem_str, sizeof(mem_str), "RAM: %luM", used_mb);
        }
    }
}

static void get_window_title(Display *dpy, Window w, char *out, size_t out_len) {
    out[0] = '\0';
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(dpy, w, atom_net_wm_name, 0, 64, False,
                           atom_utf8_string, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success && prop) {
        strncpy(out, (char *)prop, out_len - 1);
        out[out_len - 1] = '\0';
        XFree(prop);
        return;
    }

    if (XGetWindowProperty(dpy, w, XA_WM_NAME, 0, 64, False,
                           XA_STRING, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success && prop) {
        strncpy(out, (char *)prop, out_len - 1);
        out[out_len - 1] = '\0';
        XFree(prop);
        return;
    }

    strncpy(out, "Untitled", out_len - 1);
}

static int is_normal_window(Display *dpy, Window w) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(dpy, w, atom_net_wm_window_type, 0, 4, False,
                           XA_ATOM, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success && prop) {
        Atom *types = (Atom *)prop;
        for (unsigned long i = 0; i < nitems; i++) {
            if (types[i] == atom_net_wm_window_type_dock ||
                types[i] == atom_net_wm_window_type_desktop) {
                XFree(prop);
                return 0;
            }
        }
        XFree(prop);
    }
    return 1;
}

static void update_active_window(Display *dpy, Window root) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;
    active_win = None;

    if (XGetWindowProperty(dpy, root, atom_net_active_window, 0, 1, False,
                           XA_WINDOW, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success && prop) {
        if (nitems > 0) {
            active_win = *(Window *)prop;
        }
        XFree(prop);
    }
}

static void query_clients(Display *dpy, Window root, Window panel_win, int sw) {
    update_active_window(dpy, root);
    task_count = 0;

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(dpy, root, atom_net_client_list, 0, 1024, False,
                           XA_WINDOW, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success && prop) {
        Window *wins = (Window *)prop;
        for (unsigned long i = 0; i < nitems && task_count < MAX_TASKS; i++) {
            Window w = wins[i];
            if (w == None || w == 0 || w == root || w == panel_win || w == menu_win) continue;
            if (!is_normal_window(dpy, w)) continue;

            // Track window property changes
            XSelectInput(dpy, w, PropertyChangeMask | StructureNotifyMask);

            TaskItem *ti = &tasks[task_count];
            ti->win = w;
            ti->is_active = (w == active_win);
            get_window_title(dpy, w, ti->title, sizeof(ti->title));

            // Check WM_STATE for IconicState
            ti->is_minimized = 0;
            unsigned char *state_prop = NULL;
            if (XGetWindowProperty(dpy, w, atom_wm_state, 0, 2, False,
                                   atom_wm_state, &actual_type, &actual_format,
                                   &nitems, &bytes_after, &state_prop) == Success && state_prop) {
                unsigned long *s = (unsigned long *)state_prop;
                if (nitems > 0 && s[0] == IconicState) {
                    ti->is_minimized = 1;
                }
                XFree(state_prop);
            }
            task_count++;
        }
        XFree(prop);
    }

    // Layout task tabs horizontally
    int start_x = 90 + 3 * 30 + 10; // After Start button and 3 quick launch buttons
    int end_x = sw - 190;           // Space before clock and RAM indicator
    int avail_w = end_x - start_x;
    if (avail_w < 100) avail_w = 100;

    if (task_count > 0) {
        int tab_w = avail_w / task_count;
        if (tab_w > 160) tab_w = 160;
        if (tab_w < 80) tab_w = 80;

        int cur_x = start_x;
        for (int i = 0; i < task_count; i++) {
            tasks[i].x = cur_x;
            tasks[i].w = tab_w;
            cur_x += tab_w + 3;
        }
    }
}

static void draw_panel(cairo_t *cr, int sw) {
    // 1. Panel Background
    cairo_set_source_rgb(cr, 0.12, 0.14, 0.18);
    cairo_paint(cr);

    // Top subtle accent line
    cairo_set_source_rgb(cr, 0.22, 0.55, 0.85);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, 0.5);
    cairo_line_to(cr, sw, 0.5);
    cairo_stroke(cr);

    // 2. Start Button [ FreeLinX ]
    int start_w = 86;
    if (menu_open) {
        cairo_set_source_rgb(cr, 0.25, 0.55, 0.88);
    } else {
        cairo_set_source_rgb(cr, 0.18, 0.22, 0.28);
    }
    cairo_rectangle(cr, 4, 3, start_w, PANEL_H - 6);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.35, 0.65, 0.95);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 4.5, 3.5, start_w - 1.0, PANEL_H - 7.0);
    cairo_stroke(cr);

    // Cyan logo dot & text
    cairo_set_source_rgb(cr, 0.35, 0.85, 1.0);
    cairo_arc(cr, 14, PANEL_H / 2, 3.5, 0, 6.28);
    cairo_fill(cr);

    cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11.0);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_move_to(cr, 22, 18);
    cairo_show_text(cr, "FreeLinX");

    // 3. Quick Launch Buttons
    const char *ql_icons[] = {">_", "Files", "Web"};
    int ql_widths[] = {28, 38, 34};
    int ql_x = 96;
    for (int i = 0; i < 3; i++) {
        cairo_set_source_rgb(cr, 0.16, 0.19, 0.24);
        cairo_rectangle(cr, ql_x, 4, ql_widths[i], PANEL_H - 8);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 0.3, 0.35, 0.42);
        cairo_rectangle(cr, ql_x + 0.5, 4.5, ql_widths[i] - 1.0, PANEL_H - 9.0);
        cairo_stroke(cr);

        cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 10.0);
        cairo_set_source_rgb(cr, 0.85, 0.90, 0.95);
        cairo_move_to(cr, ql_x + 5, 17);
        cairo_show_text(cr, ql_icons[i]);
        ql_x += ql_widths[i] + 4;
    }

    // 4. Taskbar Window Tabs
    for (int i = 0; i < task_count; i++) {
        TaskItem *ti = &tasks[i];
        if (ti->is_active) {
            cairo_set_source_rgb(cr, 0.22, 0.45, 0.75); // Active tab blue
            cairo_rectangle(cr, ti->x, 3, ti->w, PANEL_H - 6);
            cairo_fill(cr);
            cairo_set_source_rgb(cr, 0.4, 0.7, 1.0);
            cairo_rectangle(cr, ti->x + 0.5, 3.5, ti->w - 1.0, PANEL_H - 7.0);
            cairo_stroke(cr);
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        } else {
            cairo_set_source_rgb(cr, 0.16, 0.19, 0.24);
            cairo_rectangle(cr, ti->x, 4, ti->w, PANEL_H - 8);
            cairo_fill(cr);
            cairo_set_source_rgb(cr, 0.25, 0.30, 0.36);
            cairo_rectangle(cr, ti->x + 0.5, 4.5, ti->w - 1.0, PANEL_H - 9.0);
            cairo_stroke(cr);
            if (ti->is_minimized)
                cairo_set_source_rgb(cr, 0.55, 0.60, 0.65);
            else
                cairo_set_source_rgb(cr, 0.85, 0.88, 0.92);
        }

        // Window Title
        cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL,
                               ti->is_active ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 10.0);

        char disp_title[128];
        if (ti->is_minimized)
            snprintf(disp_title, sizeof(disp_title), "[%s]", ti->title);
        else
            strncpy(disp_title, ti->title, sizeof(disp_title) - 1);
        disp_title[sizeof(disp_title) - 1] = '\0';

        // Clip title to fit tab
        cairo_text_extents_t te;
        cairo_text_extents(cr, disp_title, &te);
        while (te.width > ti->w - 14 && strlen(disp_title) > 3) {
            disp_title[strlen(disp_title) - 1] = '\0';
            cairo_text_extents(cr, disp_title, &te);
        }

        cairo_move_to(cr, ti->x + 7, 18);
        cairo_show_text(cr, disp_title);
    }

    // 5. System Tray (RAM & Clock)
    int tray_x = sw - 180;
    cairo_set_source_rgb(cr, 0.15, 0.18, 0.23);
    cairo_rectangle(cr, tray_x, 3, 176, PANEL_H - 6);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.28, 0.32, 0.40);
    cairo_rectangle(cr, tray_x + 0.5, 3.5, 175.0, PANEL_H - 7.0);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.0);
    cairo_set_source_rgb(cr, 0.35, 0.75, 0.95);
    cairo_move_to(cr, tray_x + 8, 17);
    cairo_show_text(cr, mem_str);

    cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 10.5);
    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_move_to(cr, tray_x + 82, 17);
    cairo_show_text(cr, clock_str);

    cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9.5);
    cairo_set_source_rgb(cr, 0.65, 0.70, 0.75);
    cairo_move_to(cr, tray_x + 138, 17);
    cairo_show_text(cr, date_str);
}

static void draw_menu(cairo_t *cr) {
    // Menu background
    cairo_set_source_rgb(cr, 0.14, 0.16, 0.20);
    cairo_paint(cr);

    // Border
    cairo_set_source_rgb(cr, 0.25, 0.55, 0.88);
    cairo_set_line_width(cr, 1.5);
    cairo_rectangle(cr, 0.5, 0.5, menu_w - 1.0, menu_h - 1.0);
    cairo_stroke(cr);

    // Title banner
    cairo_set_source_rgb(cr, 0.18, 0.22, 0.28);
    cairo_rectangle(cr, 1, 1, menu_w - 2, 26);
    cairo_fill(cr);

    cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11.0);
    cairo_set_source_rgb(cr, 0.35, 0.85, 1.0);
    cairo_move_to(cr, 10, 18);
    cairo_show_text(cr, "FreeLinX Applications");

    // Items
    cairo_set_font_size(cr, 10.5);
    for (int i = 0; i < MAX_MENU_ITEMS; i++) {
        int iy = 28 + i * 26;
        cairo_set_source_rgb(cr, 0.88, 0.90, 0.95);
        cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_move_to(cr, 14, iy + 17);
        cairo_show_text(cr, menu_items[i].label);

        // Divider lines
        cairo_set_source_rgba(cr, 0.3, 0.35, 0.45, 0.25);
        cairo_set_line_width(cr, 0.5);
        cairo_move_to(cr, 8, iy + 26);
        cairo_line_to(cr, menu_w - 8, iy + 26);
        cairo_stroke(cr);
    }
}

static void activate_window(Display *dpy, Window root, Window w) {
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = atom_net_active_window;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 1; // 1 = application
    ev.xclient.data.l[1] = CurrentTime;

    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XMapRaised(dpy, w);
    XSetInputFocus(dpy, w, RevertToPointerRoot, CurrentTime);
    XFlush(dpy);
}

static int x_error_handler(Display *d, XErrorEvent *e) {
    (void)d;
    (void)e;
    return 0;
}

int main(void) {
    XSetErrorHandler(x_error_handler);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "flx-panel: Cannot open X display\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    int sw = DisplayWidth(dpy, screen);
    int sh = DisplayHeight(dpy, screen);
    Visual *vis = DefaultVisual(dpy, screen);
    int depth = DefaultDepth(dpy, screen);

    // Intern Atoms
    atom_net_client_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    atom_net_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    atom_net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    atom_utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
    atom_wm_state = XInternAtom(dpy, "WM_STATE", False);
    atom_net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    atom_net_wm_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    atom_net_wm_window_type_dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    atom_net_wm_window_type_desktop = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);

    Atom atom_net_wm_strut = XInternAtom(dpy, "_NET_WM_STRUT", False);
    Atom atom_net_wm_strut_partial = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);

    // 1. Create Panel Window
    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.background_pixel = BlackPixel(dpy, screen);
    swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | StructureNotifyMask;

    Window panel_win = XCreateWindow(dpy, root, 0, sh - PANEL_H, sw, PANEL_H, 0,
                                     depth, InputOutput, vis,
                                     CWBackPixel | CWEventMask | CWOverrideRedirect, &swa);

    XStoreName(dpy, panel_win, "flx-panel");
    XClassHint ch = {"flx-panel", "FreeLinX"};
    XSetClassHint(dpy, panel_win, &ch);

    // Set Window Type to Dock
    XChangeProperty(dpy, panel_win, atom_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&atom_net_wm_window_type_dock, 1);

    // Set Struts (Reserve bottom 28px)
    unsigned long strut[4] = {0, 0, 0, PANEL_H};
    XChangeProperty(dpy, panel_win, atom_net_wm_strut, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)strut, 4);

    unsigned long strut_partial[12] = {
        0, 0, 0, PANEL_H,
        0, 0, 0, 0,
        0, 0, 0, (unsigned long)(sw - 1)
    };
    XChangeProperty(dpy, panel_win, atom_net_wm_strut_partial, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)strut_partial, 12);

    XMapWindow(dpy, panel_win);

    // 2. Create Start Menu Window (Popup, override-redirect)
    XSetWindowAttributes m_swa;
    m_swa.override_redirect = True;
    m_swa.background_pixel = BlackPixel(dpy, screen);
    m_swa.event_mask = ExposureMask | ButtonPressMask;

    menu_win = XCreateWindow(dpy, root, 4, sh - PANEL_H - menu_h - 2, menu_w, menu_h, 0,
                             depth, InputOutput, vis,
                             CWBackPixel | CWEventMask | CWOverrideRedirect, &m_swa);

    // Double buffer for panel
    Pixmap panel_backbuffer = XCreatePixmap(dpy, panel_win, sw, PANEL_H, depth);
    GC gc = XCreateGC(dpy, panel_win, 0, NULL);
    cairo_surface_t *panel_cs = cairo_xlib_surface_create(dpy, panel_backbuffer, vis, sw, PANEL_H);
    cairo_t *panel_cr = cairo_create(panel_cs);

    // Double buffer for menu
    menu_backbuffer = XCreatePixmap(dpy, menu_win, menu_w, menu_h, depth);
    menu_cs = cairo_xlib_surface_create(dpy, menu_backbuffer, vis, menu_w, menu_h);
    menu_cr = cairo_create(menu_cs);

    // Listen to root property changes (window open/close/focus)
    XSelectInput(dpy, root, PropertyChangeMask | StructureNotifyMask | SubstructureNotifyMask);

    update_sysinfo();
    query_clients(dpy, root, panel_win, sw);
    draw_panel(panel_cr, sw);
    XCopyArea(dpy, panel_backbuffer, panel_win, gc, 0, 0, sw, PANEL_H, 0, 0);
    XFlush(dpy);

    int xfd = ConnectionNumber(dpy);
    struct timespec last_update;
    clock_gettime(CLOCK_MONOTONIC, &last_update);

    int running = 1;
    while (running) {
        while (XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            if (ev.type == Expose) {
                if (ev.xexpose.window == panel_win && ev.xexpose.count == 0) {
                    XCopyArea(dpy, panel_backbuffer, panel_win, gc, 0, 0, sw, PANEL_H, 0, 0);
                    XFlush(dpy);
                } else if (ev.xexpose.window == menu_win && ev.xexpose.count == 0 && menu_open) {
                    draw_menu(menu_cr);
                    XCopyArea(dpy, menu_backbuffer, menu_win, gc, 0, 0, menu_w, menu_h, 0, 0);
                    XFlush(dpy);
                }
            } else if (ev.type == PropertyNotify) {
                if (ev.xproperty.window == root) {
                    if (ev.xproperty.atom == atom_net_client_list ||
                        ev.xproperty.atom == atom_net_active_window) {
                        query_clients(dpy, root, panel_win, sw);
                        draw_panel(panel_cr, sw);
                        XCopyArea(dpy, panel_backbuffer, panel_win, gc, 0, 0, sw, PANEL_H, 0, 0);
                        XFlush(dpy);
                    }
                } else {
                    if (ev.xproperty.atom == atom_net_wm_name ||
                        ev.xproperty.atom == XA_WM_NAME ||
                        ev.xproperty.atom == atom_wm_state) {
                        query_clients(dpy, root, panel_win, sw);
                        draw_panel(panel_cr, sw);
                        XCopyArea(dpy, panel_backbuffer, panel_win, gc, 0, 0, sw, PANEL_H, 0, 0);
                        XFlush(dpy);
                    }
                }
            } else if (ev.type == DestroyNotify) {
                query_clients(dpy, root, panel_win, sw);
                draw_panel(panel_cr, sw);
                XCopyArea(dpy, panel_backbuffer, panel_win, gc, 0, 0, sw, PANEL_H, 0, 0);
                XFlush(dpy);
            } else if (ev.type == ButtonPress) {
                if (ev.xbutton.window == panel_win) {
                    int mx = ev.xbutton.x;

                    // 1. Check Start Button
                    if (mx >= 4 && mx < 90) {
                        menu_open = !menu_open;
                        if (menu_open) {
                            XMapRaised(dpy, menu_win);
                            draw_menu(menu_cr);
                            XCopyArea(dpy, menu_backbuffer, menu_win, gc, 0, 0, menu_w, menu_h, 0, 0);
                        } else {
                            XUnmapWindow(dpy, menu_win);
                        }
                        draw_panel(panel_cr, sw);
                        XCopyArea(dpy, panel_backbuffer, panel_win, gc, 0, 0, sw, PANEL_H, 0, 0);
                        XFlush(dpy);
                        continue;
                    }

                    // 2. Check Quick Launch Buttons
                    if (mx >= 96 && mx < 96 + 28) {
                        system("uxterm &");
                    } else if (mx >= 96 + 32 && mx < 96 + 32 + 38) {
                        system("flx-fm &");
                    } else if (mx >= 96 + 74 && mx < 96 + 74 + 34) {
                        system("flx-browser &");
                    }

                    // 3. Check Task Tabs
                    for (int i = 0; i < task_count; i++) {
                        TaskItem *ti = &tasks[i];
                        if (mx >= ti->x && mx < ti->x + ti->w) {
                            if (ti->is_active) {
                                // Minimize
                                XIconifyWindow(dpy, ti->win, screen);
                            } else {
                                // Activate and unminimize
                                activate_window(dpy, root, ti->win);
                            }
                            query_clients(dpy, root, panel_win, sw);
                            draw_panel(panel_cr, sw);
                            XCopyArea(dpy, panel_backbuffer, panel_win, gc, 0, 0, sw, PANEL_H, 0, 0);
                            XFlush(dpy);
                            break;
                        }
                    }

                    // Close menu if clicking elsewhere on panel
                    if (menu_open) {
                        menu_open = 0;
                        XUnmapWindow(dpy, menu_win);
                        draw_panel(panel_cr, sw);
                        XCopyArea(dpy, panel_backbuffer, panel_win, gc, 0, 0, sw, PANEL_H, 0, 0);
                        XFlush(dpy);
                    }
                } else if (ev.xbutton.window == menu_win && menu_open) {
                    int my = ev.xbutton.y;
                    if (my >= 28) {
                        int idx = (my - 28) / 26;
                        if (idx >= 0 && idx < MAX_MENU_ITEMS) {
                            system(menu_items[idx].cmd);
                        }
                    }
                    menu_open = 0;
                    XUnmapWindow(dpy, menu_win);
                    draw_panel(panel_cr, sw);
                    XCopyArea(dpy, panel_backbuffer, panel_win, gc, 0, 0, sw, PANEL_H, 0, 0);
                    XFlush(dpy);
                }
            }
        }

        // Wait on X connection or 1-second timeout
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv = {1, 0};
        select(xfd + 1, &fds, NULL, NULL, &tv);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - last_update.tv_sec >= 1) {
            last_update = now;
            update_sysinfo();
            query_clients(dpy, root, panel_win, sw);
            draw_panel(panel_cr, sw);
            XCopyArea(dpy, panel_backbuffer, panel_win, gc, 0, 0, sw, PANEL_H, 0, 0);
            XFlush(dpy);
        }
    }

    cairo_destroy(panel_cr);
    cairo_surface_destroy(panel_cs);
    XFreePixmap(dpy, panel_backbuffer);
    cairo_destroy(menu_cr);
    cairo_surface_destroy(menu_cs);
    XFreePixmap(dpy, menu_backbuffer);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, menu_win);
    XDestroyWindow(dpy, panel_win);
    XCloseDisplay(dpy);
    return 0;
}
