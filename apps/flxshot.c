#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>

static unsigned char get_channel(unsigned long pixel, unsigned long mask) {
    if (!mask) return 0;
    int shift = 0;
    unsigned long m = mask;
    while ((m & 1) == 0) {
        shift++;
        m >>= 1;
    }
    if (m == 0) return 0;
    return (unsigned char)(((pixel & mask) >> shift) * 255 / m);
}

static void print_usage(const char *prog) {
    fprintf(stderr, "FreeLinX Screenshot Tool\n");
    fprintf(stderr, "Usage: %s [OPTIONS] [output.png]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -d <sec>   Delay in seconds before capturing\n");
    fprintf(stderr, "  -h         Show this help message\n");
}

int main(int argc, char **argv) {
    int delay_sec = 0;
    const char *out_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            delay_sec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            out_path = argv[i];
        }
    }

    if (delay_sec > 0) {
        sleep(delay_sec);
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "flxshot: Cannot open X display\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    int sw = DisplayWidth(dpy, screen);
    int sh = DisplayHeight(dpy, screen);

    XImage *ximg = XGetImage(dpy, root, 0, 0, sw, sh, AllPlanes, ZPixmap);
    if (!ximg) {
        fprintf(stderr, "flxshot: Failed to capture X11 root window\n");
        XCloseDisplay(dpy);
        return 1;
    }

    char default_filename[256];
    if (!out_path) {
        const char *home = getenv("HOME");
        if (!home) home = "/root";

        char shot_dir[256];
        snprintf(shot_dir, sizeof(shot_dir), "%s/screenshots", home);
        mkdir(shot_dir, 0755);

        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char timebuf[64];
        if (tm) {
            strftime(timebuf, sizeof(timebuf), "%Y-%m-%d_%H-%M-%S", tm);
        } else {
            snprintf(timebuf, sizeof(timebuf), "%ld", (long)now);
        }

        snprintf(default_filename, sizeof(default_filename), "%s/screenshot_%s.png", shot_dir, timebuf);
        out_path = default_filename;
    }

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, sw, sh);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "flxshot: Failed to create Cairo surface\n");
        XDestroyImage(ximg);
        XCloseDisplay(dpy);
        return 1;
    }

    unsigned char *dst_data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);

    for (int y = 0; y < sh; y++) {
        uint32_t *row = (uint32_t *)(dst_data + y * stride);
        for (int x = 0; x < sw; x++) {
            unsigned long pix = XGetPixel(ximg, x, y);
            unsigned char r = get_channel(pix, ximg->red_mask);
            unsigned char g = get_channel(pix, ximg->green_mask);
            unsigned char b = get_channel(pix, ximg->blue_mask);
            row[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    cairo_surface_mark_dirty(surface);
    cairo_status_t status = cairo_surface_write_to_png(surface, out_path);
    cairo_surface_destroy(surface);
    XDestroyImage(ximg);

    if (status != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "flxshot: Failed to save PNG to '%s': %s\n",
                out_path, cairo_status_to_string(status));
        XCloseDisplay(dpy);
        return 1;
    }

    printf("Screenshot saved to %s\n", out_path);
    XBell(dpy, 0);
    XFlush(dpy);
    XCloseDisplay(dpy);
    return 0;
}
