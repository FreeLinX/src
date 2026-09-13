#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

int main(int argc, char **argv) {
    const char *path = "/usr/share/backgrounds/freelinx-default.png";
    if (argc > 1 && argv[1][0] != '\0') {
        path = argv[1];
    }

    if (access(path, R_OK) != 0) {
        fprintf(stderr, "flx-bg: Cannot read wallpaper file '%s'\n", path);
        return 1;
    }

    cairo_surface_t *img = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "flx-bg: Failed to load PNG image '%s'\n", path);
        return 1;
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "flx-bg: Cannot open X display\n");
        cairo_surface_destroy(img);
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

    // Dark charcoal fallback background
    cairo_set_source_rgb(cr, 0.08, 0.10, 0.14);
    cairo_paint(cr);

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

    cairo_destroy(cr);
    cairo_surface_destroy(cs);
    cairo_surface_destroy(img);

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
