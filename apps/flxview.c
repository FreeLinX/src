#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#define TOOLBAR_H 34
#define STATUS_H 24
#define MAX_DIR_FILES 256

static int win_w = 800;
static int win_h = 600;

static cairo_surface_t *img_surf = NULL;
static int img_w = 0;
static int img_h = 0;
static char current_file[512] = "";
static char dir_path[512] = "";

static char dir_images[MAX_DIR_FILES][256];
static int dir_image_count = 0;
static int current_dir_idx = -1;

static double zoom_scale = 1.0;
static double pan_x = 0.0;
static double pan_y = 0.0;
static int is_dragging = 0;
static int drag_start_x = 0;
static int drag_start_y = 0;
static double drag_start_pan_x = 0.0;
static double drag_start_pan_y = 0.0;

// Open Prompt
static int prompt_open = 0;
static char prompt_buf[512] = "";
static int prompt_cur = 0;

static int is_image_ext(const char *fn) {
    const char *dot = strrchr(fn, '.');
    if (!dot) return 0;
    if (strcasecmp(dot, ".png") == 0) return 1;
    if (strcasecmp(dot, ".jpg") == 0) return 1;
    if (strcasecmp(dot, ".jpeg") == 0) return 1;
    if (strcasecmp(dot, ".bmp") == 0) return 1;
    return 0;
}

static void scan_directory(const char *filepath) {
    dir_image_count = 0;
    current_dir_idx = -1;

    const char *slash = strrchr(filepath, '/');
    if (slash) {
        int dlen = slash - filepath;
        if (dlen == 0) dlen = 1;
        strncpy(dir_path, filepath, dlen);
        dir_path[dlen] = '\0';
    } else {
        strcpy(dir_path, ".");
    }

    DIR *d = opendir(dir_path);
    if (!d) return;

    struct dirent *ent;
    const char *target_base = slash ? slash + 1 : filepath;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (is_image_ext(ent->d_name)) {
            if (dir_image_count < MAX_DIR_FILES) {
                strncpy(dir_images[dir_image_count], ent->d_name, sizeof(dir_images[0]) - 1);
                dir_images[dir_image_count][sizeof(dir_images[0]) - 1] = '\0';
                if (strcmp(ent->d_name, target_base) == 0) {
                    current_dir_idx = dir_image_count;
                }
                dir_image_count++;
            }
        }
    }
    closedir(d);
}

static void fit_image(void) {
    if (!img_surf || img_w <= 0 || img_h <= 0) return;
    int view_w = win_w;
    int view_h = win_h - TOOLBAR_H - STATUS_H;
    if (view_w <= 0 || view_h <= 0) return;

    double sx = (double)(view_w - 40) / (double)img_w;
    double sy = (double)(view_h - 40) / (double)img_h;
    zoom_scale = (sx < sy) ? sx : sy;
    if (zoom_scale > 1.0) zoom_scale = 1.0; // Don't upscale small images automatically

    pan_x = (view_w - img_w * zoom_scale) / 2.0;
    pan_y = (view_h - img_h * zoom_scale) / 2.0;
}

static int load_image(const char *path) {
    if (img_surf) {
        cairo_surface_destroy(img_surf);
        img_surf = NULL;
    }

    img_surf = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(img_surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(img_surf);
        img_surf = NULL;
        return 0;
    }

    img_w = cairo_image_surface_get_width(img_surf);
    img_h = cairo_image_surface_get_height(img_surf);
    strncpy(current_file, path, sizeof(current_file) - 1);
    current_file[sizeof(current_file) - 1] = '\0';

    scan_directory(path);
    fit_image();
    return 1;
}

static void switch_image_offset(int offset) {
    if (dir_image_count <= 0) return;
    current_dir_idx = (current_dir_idx + offset + dir_image_count) % dir_image_count;
    char full[512];
    if (strcmp(dir_path, "/") == 0) {
        snprintf(full, sizeof(full), "/%s", dir_images[current_dir_idx]);
    } else {
        snprintf(full, sizeof(full), "%s/%s", dir_path, dir_images[current_dir_idx]);
    }
    load_image(full);
}

static void draw_viewer(cairo_t *cr, Display *dpy, Window win) {
    // Window Title
    char title[600];
    if (current_file[0]) {
        snprintf(title, sizeof(title), "%s (%dx%d, %d%%) - flxview",
                 current_file, img_w, img_h, (int)(zoom_scale * 100));
    } else {
        snprintf(title, sizeof(title), "FreeLinX Image Viewer - flxview");
    }
    XStoreName(dpy, win, title);

    // 1. Background (Dark checkerboard pattern)
    cairo_set_source_rgb(cr, 0.12, 0.14, 0.17);
    cairo_paint(cr);

    int view_y = TOOLBAR_H;
    int view_h = win_h - TOOLBAR_H - STATUS_H;

    // Checker pattern
    cairo_save(cr);
    cairo_rectangle(cr, 0, view_y, win_w, view_h);
    cairo_clip(cr);
    cairo_set_source_rgb(cr, 0.15, 0.17, 0.21);
    for (int y = view_y; y < view_y + view_h; y += 16) {
        for (int x = 0; x < win_w; x += 16) {
            if (((x / 16) + (y / 16)) % 2 == 0) {
                cairo_rectangle(cr, x, y, 16, 16);
                cairo_fill(cr);
            }
        }
    }

    // 2. Render Image
    if (img_surf) {
        cairo_save(cr);
        cairo_translate(cr, pan_x, view_y + pan_y);
        cairo_scale(cr, zoom_scale, zoom_scale);
        cairo_set_source_surface(cr, img_surf, 0, 0);
        cairo_paint(cr);

        // Subtle border around image
        cairo_set_source_rgba(cr, 0.3, 0.5, 0.8, 0.4);
        cairo_set_line_width(cr, 1.0 / zoom_scale);
        cairo_rectangle(cr, 0, 0, img_w, img_h);
        cairo_stroke(cr);

        cairo_restore(cr);
    } else {
        // No image loaded placeholder
        cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 14.0);
        cairo_set_source_rgb(cr, 0.50, 0.60, 0.75);
        cairo_move_to(cr, win_w / 2 - 120, win_h / 2 - 10);
        cairo_show_text(cr, "No image loaded");

        cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.0);
        cairo_set_source_rgb(cr, 0.40, 0.48, 0.58);
        cairo_move_to(cr, win_w / 2 - 150, win_h / 2 + 16);
        cairo_show_text(cr, "Click Open or pass image path to flxview");
    }
    cairo_restore(cr);

    // 3. Top Toolbar
    cairo_set_source_rgb(cr, 0.16, 0.19, 0.25);
    cairo_rectangle(cr, 0, 0, win_w, TOOLBAR_H);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.24, 0.30, 0.40);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, TOOLBAR_H - 0.5);
    cairo_line_to(cr, win_w, TOOLBAR_H - 0.5);
    cairo_stroke(cr);

    const char *btns[] = {"Open", "Fit", "100%", "Zoom +", "Zoom -", "< Prev", "Next >"};
    int btn_w[] = {52, 44, 50, 60, 60, 56, 56};
    int bx = 8;
    for (int i = 0; i < 7; i++) {
        cairo_set_source_rgb(cr, 0.22, 0.26, 0.34);
        cairo_rectangle(cr, bx, 5, btn_w[i], TOOLBAR_H - 10);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 0.35, 0.42, 0.55);
        cairo_rectangle(cr, bx + 0.5, 5.5, btn_w[i] - 1.0, TOOLBAR_H - 11.0);
        cairo_stroke(cr);

        cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 10.5);
        cairo_set_source_rgb(cr, 0.90, 0.93, 0.98);
        cairo_move_to(cr, bx + 8, 21);
        cairo_show_text(cr, btns[i]);

        bx += btn_w[i] + 6;
    }

    // 4. Bottom Status Bar
    int sb_y = win_h - STATUS_H;
    cairo_set_source_rgb(cr, 0.14, 0.17, 0.22);
    cairo_rectangle(cr, 0, sb_y, win_w, STATUS_H);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.22, 0.26, 0.35);
    cairo_move_to(cr, 0, sb_y + 0.5);
    cairo_line_to(cr, win_w, sb_y + 0.5);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.5);
    cairo_set_source_rgb(cr, 0.80, 0.85, 0.90);
    cairo_move_to(cr, 12, sb_y + 16);
    if (current_file[0]) {
        cairo_show_text(cr, current_file);
    } else {
        cairo_show_text(cr, "No file open");
    }

    // Right status info
    char rinfo[128];
    if (img_surf) {
        snprintf(rinfo, sizeof(rinfo), "%dx%d px   Zoom: %d%%   [%d/%d]",
                 img_w, img_h, (int)(zoom_scale * 100),
                 current_dir_idx >= 0 ? current_dir_idx + 1 : 1,
                 dir_image_count > 0 ? dir_image_count : 1);
    } else {
        snprintf(rinfo, sizeof(rinfo), "0x0 px");
    }
    cairo_text_extents_t rte;
    cairo_text_extents(cr, rinfo, &rte);
    cairo_move_to(cr, win_w - rte.width - 14, sb_y + 16);
    cairo_set_source_rgb(cr, 0.40, 0.85, 1.0);
    cairo_show_text(cr, rinfo);

    // 5. Open Modal Prompt
    if (prompt_open) {
        int pw = 480;
        int ph = 90;
        int px = (win_w - pw) / 2;
        int py = (win_h - ph) / 2;

        cairo_set_source_rgba(cr, 0.05, 0.07, 0.10, 0.70);
        cairo_rectangle(cr, 0, 0, win_w, win_h);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 0.16, 0.19, 0.26);
        cairo_rectangle(cr, px, py, pw, ph);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 0.30, 0.60, 0.95);
        cairo_set_line_width(cr, 1.5);
        cairo_rectangle(cr, px + 0.5, py + 0.5, pw - 1.0, ph - 1.0);
        cairo_stroke(cr);

        cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11.0);
        cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
        cairo_move_to(cr, px + 14, py + 24);
        cairo_show_text(cr, "Open Image (Enter = OK, Esc = Cancel):");

        cairo_set_source_rgb(cr, 0.10, 0.12, 0.16);
        cairo_rectangle(cr, px + 14, py + 38, pw - 28, 28);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 0.35, 0.45, 0.60);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, px + 14.5, py + 38.5, pw - 29.0, 27.0);
        cairo_stroke(cr);

        cairo_select_font_face(cr, "DejaVu Sans Mono", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.5);
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_move_to(cr, px + 22, py + 56);
        cairo_show_text(cr, prompt_buf);
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0] != '\0') {
        load_image(argv[1]);
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "flxview: Cannot open X display\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    Visual *vis = DefaultVisual(dpy, screen);
    int depth = DefaultDepth(dpy, screen);

    XSetWindowAttributes swa;
    swa.background_pixel = BlackPixel(dpy, screen);
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | StructureNotifyMask;

    Window win = XCreateWindow(dpy, root, 100, 60, win_w, win_h, 0,
                               depth, InputOutput, vis,
                               CWBackPixel | CWEventMask, &swa);

    XStoreName(dpy, win, "flxview");
    XClassHint ch = {"flxview", "FreeLinX"};
    XSetClassHint(dpy, win, &ch);

    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    Pixmap backbuffer = XCreatePixmap(dpy, win, win_w, win_h, depth);
    GC gc = XCreateGC(dpy, win, 0, NULL);
    cairo_surface_t *cs = cairo_xlib_surface_create(dpy, backbuffer, vis, win_w, win_h);
    cairo_t *cr = cairo_create(cs);

    XMapWindow(dpy, win);

    int running = 1;
    while (running) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        if (ev.type == Expose && ev.xexpose.count == 0) {
            draw_viewer(cr, dpy, win);
            XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
            XFlush(dpy);
        } else if (ev.type == ConfigureNotify) {
            if (ev.xconfigure.width != win_w || ev.xconfigure.height != win_h) {
                win_w = ev.xconfigure.width;
                win_h = ev.xconfigure.height;
                cairo_destroy(cr);
                cairo_surface_destroy(cs);
                XFreePixmap(dpy, backbuffer);

                backbuffer = XCreatePixmap(dpy, win, win_w, win_h, depth);
                cs = cairo_xlib_surface_create(dpy, backbuffer, vis, win_w, win_h);
                cr = cairo_create(cs);

                draw_viewer(cr, dpy, win);
                XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
                XFlush(dpy);
            }
        } else if (ev.type == ClientMessage) {
            if ((Atom)ev.xclient.data.l[0] == wm_delete) {
                running = 0;
            }
        } else if (ev.type == ButtonPress) {
            int mx = ev.xbutton.x;
            int my = ev.xbutton.y;

            if (prompt_open) {
                prompt_open = 0;
                draw_viewer(cr, dpy, win);
                XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
                XFlush(dpy);
                continue;
            }

            if (my < TOOLBAR_H) {
                int btn_w[] = {52, 44, 50, 60, 60, 56, 56};
                int bx = 8;
                for (int i = 0; i < 7; i++) {
                    if (mx >= bx && mx < bx + btn_w[i]) {
                        switch (i) {
                            case 0: // Open
                                prompt_open = 1;
                                prompt_buf[0] = '\0';
                                prompt_cur = 0;
                                break;
                            case 1: // Fit
                                fit_image();
                                break;
                            case 2: // 100%
                                zoom_scale = 1.0;
                                pan_x = (win_w - img_w) / 2.0;
                                pan_y = (win_h - TOOLBAR_H - STATUS_H - img_h) / 2.0;
                                break;
                            case 3: // Zoom +
                                zoom_scale *= 1.25;
                                break;
                            case 4: // Zoom -
                                zoom_scale *= 0.8;
                                break;
                            case 5: // Prev
                                switch_image_offset(-1);
                                break;
                            case 6: // Next
                                switch_image_offset(1);
                                break;
                        }
                        break;
                    }
                    bx += btn_w[i] + 6;
                }
            } else if (ev.xbutton.button == 4) { // Scroll Up -> Zoom In
                double old_zoom = zoom_scale;
                zoom_scale *= 1.15;
                pan_x = mx - (mx - pan_x) * (zoom_scale / old_zoom);
                pan_y = (my - TOOLBAR_H) - ((my - TOOLBAR_H) - pan_y) * (zoom_scale / old_zoom);
            } else if (ev.xbutton.button == 5) { // Scroll Down -> Zoom Out
                double old_zoom = zoom_scale;
                zoom_scale *= 0.85;
                pan_x = mx - (mx - pan_x) * (zoom_scale / old_zoom);
                pan_y = (my - TOOLBAR_H) - ((my - TOOLBAR_H) - pan_y) * (zoom_scale / old_zoom);
            } else if (ev.xbutton.button == 1 && my >= TOOLBAR_H && my < win_h - STATUS_H) {
                is_dragging = 1;
                drag_start_x = mx;
                drag_start_y = my;
                drag_start_pan_x = pan_x;
                drag_start_pan_y = pan_y;
            }

            draw_viewer(cr, dpy, win);
            XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
            XFlush(dpy);
        } else if (ev.type == ButtonRelease) {
            if (ev.xbutton.button == 1) {
                is_dragging = 0;
            }
        } else if (ev.type == MotionNotify) {
            if (is_dragging) {
                pan_x = drag_start_pan_x + (ev.xmotion.x - drag_start_x);
                pan_y = drag_start_pan_y + (ev.xmotion.y - drag_start_y);
                draw_viewer(cr, dpy, win);
                XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
                XFlush(dpy);
            }
        } else if (ev.type == KeyPress) {
            char kbuf[32];
            KeySym ksym;
            int nchars = XLookupString(&ev.xkey, kbuf, sizeof(kbuf) - 1, &ksym, NULL);
            kbuf[nchars] = '\0';

            if (prompt_open) {
                if (ksym == XK_Escape) {
                    prompt_open = 0;
                } else if (ksym == XK_Return) {
                    if (prompt_buf[0]) {
                        load_image(prompt_buf);
                    }
                    prompt_open = 0;
                } else if (ksym == XK_BackSpace) {
                    if (prompt_cur > 0) {
                        prompt_buf[--prompt_cur] = '\0';
                    }
                } else if (nchars > 0 && kbuf[0] >= 32) {
                    if (prompt_cur + 1 < (int)sizeof(prompt_buf) - 1) {
                        prompt_buf[prompt_cur++] = kbuf[0];
                        prompt_buf[prompt_cur] = '\0';
                    }
                }
            } else {
                if (ksym == XK_Escape || ksym == XK_q || ksym == XK_Q) {
                    running = 0;
                } else if (ksym == XK_plus || ksym == XK_equal || ksym == XK_KP_Add) {
                    zoom_scale *= 1.25;
                } else if (ksym == XK_minus || ksym == XK_KP_Subtract) {
                    zoom_scale *= 0.8;
                } else if (ksym == XK_0 || ksym == XK_f || ksym == XK_F) {
                    fit_image();
                } else if (ksym == XK_1) {
                    zoom_scale = 1.0;
                    pan_x = (win_w - img_w) / 2.0;
                    pan_y = (win_h - TOOLBAR_H - STATUS_H - img_h) / 2.0;
                } else if (ksym == XK_Left || ksym == XK_p || ksym == XK_P) {
                    switch_image_offset(-1);
                } else if (ksym == XK_Right || ksym == XK_n || ksym == XK_N) {
                    switch_image_offset(1);
                } else if (ksym == XK_o || ksym == XK_O) {
                    prompt_open = 1;
                    prompt_buf[0] = '\0';
                    prompt_cur = 0;
                }
            }

            draw_viewer(cr, dpy, win);
            XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
            XFlush(dpy);
        }
    }

    if (img_surf) cairo_surface_destroy(img_surf);
    cairo_destroy(cr);
    cairo_surface_destroy(cs);
    XFreePixmap(dpy, backbuffer);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
