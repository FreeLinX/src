#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

static int parse_hex_color(const char *str, double *r, double *g, double *b) {
    if (!str) return 0;
    if (str[0] == '#') str++;
    if (strlen(str) != 6) return 0;
    unsigned int hex = 0;
    if (sscanf(str, "%x", &hex) != 1) return 0;
    *r = ((hex >> 16) & 0xFF) / 255.0;
    *g = ((hex >> 8) & 0xFF) / 255.0;
    *b = (hex & 0xFF) / 255.0;
    return 1;
}

int main(int argc, char **argv) {
    // Default flat solid color: modern dark slate charcoal
    double cr_r = 0.125, cr_g = 0.141, cr_b = 0.173; // #20242c
    int is_solid = 1;
    const char *img_path = NULL;

    if (argc > 1 && argv[1][0] != '\0') {
        if (argv[1][0] == '#' || parse_hex_color(argv[1], &cr_r, &cr_g, &cr_b)) {
            is_solid = 1;
            parse_hex_color(argv[1], &cr_r, &cr_g, &cr_b);
        } else if (access(argv[1], R_OK) == 0) {
            is_solid = 0;
            img_path = argv[1];
        } else {
            // Try parsing as hex without #
            if (parse_hex_color(argv[1], &cr_r, &cr_g, &cr_b)) {
                is_solid = 1;
            } else {
                fprintf(stderr, "flxbg: Unrecognized color or missing file '%s', using default solid color\n", argv[1]);
                is_solid = 1;
            }
        }
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "flxbg: Cannot open X display\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    int sw = DisplayWidth(dpy, screen);
    int sh = DisplayHeight(dpy, screen);
    int depth = DefaultDepth(dpy, screen);
    Visual *vis = DefaultVisual(dpy, screen);

    Pixmap pmap = XCreatePixmap(dpy, root, sw, sh, depth);
    cairo_surface_t *cs = cairo_xlib_surface_create(dpy, pmap, vis, sw, sh);
    cairo_t *cr = cairo_create(cs);

    // Fill with solid color
    cairo_set_source_rgb(cr, cr_r, cr_g, cr_b);
    cairo_paint(cr);

    if (!is_solid && img_path) {
        cairo_surface_t *img = cairo_image_surface_create_from_png(img_path);
        if (cairo_surface_status(img) == CAIRO_STATUS_SUCCESS) {
            int iw = cairo_image_surface_get_width(img);
            int ih = cairo_image_surface_get_height(img);

            double sx = (double)sw / (double)iw;
            double sy = (double)sh / (double)ih;
            double scale = (sx > sy) ? sx : sy; // Aspect cover

            double ox = (sw - iw * scale) / 2.0;
            double oy = (sh - ih * scale) / 2.0;

            cairo_save(cr);
            cairo_translate(cr, ox, oy);
            cairo_scale(cr, scale, scale);
            cairo_set_source_surface(cr, img, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);

            cairo_surface_destroy(img);
        }
    }

    cairo_destroy(cr);
    cairo_surface_destroy(cs);

    // Set standard X11 root pixmap atoms (for pseudo-transparency & window managers)
    Atom atom_root = XInternAtom(dpy, "_XROOTPMAP_ID", False);
    Atom atom_eset = XInternAtom(dpy, "ESETROOT_PMAP_ID", False);

    XChangeProperty(dpy, root, atom_root, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&pmap, 1);
    XChangeProperty(dpy, root, atom_eset, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&pmap, 1);

    XSetWindowBackgroundPixmap(dpy, root, pmap);
    XClearWindow(dpy, root);

    // Retain pixmap after process exits
    XSetCloseDownMode(dpy, RetainPermanent);
    XFlush(dpy);
    XCloseDisplay(dpy);

    return 0;
}
