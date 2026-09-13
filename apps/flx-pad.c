#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#define TOOLBAR_H 34
#define STATUS_H 24
#define LINE_H 18
#define FONT_SIZE 12.0
#define GUTTER_W 50

typedef struct {
    char *chars;
    int len;
    int cap;
} Line;

typedef struct {
    Line *lines;
    int count;
    int cap;
} Buffer;

static Buffer buf;
static char current_filepath[512] = "";
static int is_dirty = 0;

static int cur_line = 0;
static int cur_col = 0;
static int scroll_line = 0;
static int scroll_col = 0;

static int win_w = 720;
static int win_h = 520;
static char status_msg[128] = "Ready";

// Modal prompt state
typedef enum { PROMPT_NONE, PROMPT_OPEN, PROMPT_SAVE_AS } PromptMode;
static PromptMode prompt_mode = PROMPT_NONE;
static char prompt_input[512] = "";
static int prompt_cur = 0;

static void buf_init(void) {
    buf.cap = 64;
    buf.count = 1;
    buf.lines = malloc(sizeof(Line) * buf.cap);
    buf.lines[0].cap = 32;
    buf.lines[0].len = 0;
    buf.lines[0].chars = malloc(32);
    buf.lines[0].chars[0] = '\0';
}

static void buf_free(void) {
    for (int i = 0; i < buf.count; i++) {
        free(buf.lines[i].chars);
    }
    free(buf.lines);
}

static void buf_clear(void) {
    buf_free();
    buf_init();
    cur_line = 0;
    cur_col = 0;
    scroll_line = 0;
    scroll_col = 0;
    is_dirty = 0;
    current_filepath[0] = '\0';
}

static void line_insert_char(Line *l, int pos, char c) {
    if (pos < 0) pos = 0;
    if (pos > l->len) pos = l->len;
    if (l->len + 2 >= l->cap) {
        l->cap = (l->cap < 16) ? 32 : l->cap * 2;
        l->chars = realloc(l->chars, l->cap);
    }
    memmove(l->chars + pos + 1, l->chars + pos, l->len - pos);
    l->chars[pos] = c;
    l->len++;
    l->chars[l->len] = '\0';
}

static void line_delete_char(Line *l, int pos) {
    if (pos < 0 || pos >= l->len) return;
    memmove(l->chars + pos, l->chars + pos + 1, l->len - pos);
    l->len--;
    l->chars[l->len] = '\0';
}

static void buf_insert_line(int idx) {
    if (buf.count + 1 >= buf.cap) {
        buf.cap *= 2;
        buf.lines = realloc(buf.lines, sizeof(Line) * buf.cap);
    }
    memmove(buf.lines + idx + 1, buf.lines + idx, sizeof(Line) * (buf.count - idx));
    buf.lines[idx].cap = 32;
    buf.lines[idx].len = 0;
    buf.lines[idx].chars = malloc(32);
    buf.lines[idx].chars[0] = '\0';
    buf.count++;
}

static void buf_remove_line(int idx) {
    if (idx < 0 || idx >= buf.count) return;
    free(buf.lines[idx].chars);
    memmove(buf.lines + idx, buf.lines + idx + 1, sizeof(Line) * (buf.count - idx - 1));
    buf.count--;
}

static int buf_load_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    buf_free();
    buf.cap = 128;
    buf.count = 0;
    buf.lines = malloc(sizeof(Line) * buf.cap);

    char *line_buf = NULL;
    size_t line_cap = 0;
    ssize_t nread;

    while ((nread = getline(&line_buf, &line_cap, f)) != -1) {
        if (nread > 0 && line_buf[nread - 1] == '\n') {
            nread--;
            if (nread > 0 && line_buf[nread - 1] == '\r') {
                nread--;
            }
        }
        if (buf.count >= buf.cap) {
            buf.cap *= 2;
            buf.lines = realloc(buf.lines, sizeof(Line) * buf.cap);
        }
        Line *l = &buf.lines[buf.count];
        l->cap = nread + 16;
        l->len = nread;
        l->chars = malloc(l->cap);
        memcpy(l->chars, line_buf, nread);
        l->chars[nread] = '\0';
        buf.count++;
    }
    free(line_buf);
    fclose(f);

    if (buf.count == 0) {
        buf_init();
    }

    strncpy(current_filepath, path, sizeof(current_filepath) - 1);
    current_filepath[sizeof(current_filepath) - 1] = '\0';
    cur_line = 0;
    cur_col = 0;
    scroll_line = 0;
    scroll_col = 0;
    is_dirty = 0;
    snprintf(status_msg, sizeof(status_msg), "Opened '%s' (%d lines)", path, buf.count);
    return 1;
}

static int buf_save_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(status_msg, sizeof(status_msg), "Error: Cannot write to '%s'", path);
        return 0;
    }
    for (int i = 0; i < buf.count; i++) {
        fputs(buf.lines[i].chars, f);
        fputc('\n', f);
    }
    fclose(f);
    strncpy(current_filepath, path, sizeof(current_filepath) - 1);
    is_dirty = 0;
    snprintf(status_msg, sizeof(status_msg), "Saved to '%s' (%d lines)", path, buf.count);
    return 1;
}

static void ensure_cursor_visible(int visible_lines) {
    if (cur_line < scroll_line) {
        scroll_line = cur_line;
    }
    if (cur_line >= scroll_line + visible_lines) {
        scroll_line = cur_line - visible_lines + 1;
    }
    if (cur_col < scroll_col) {
        scroll_col = cur_col;
    }
    if (cur_col >= scroll_col + 80) {
        scroll_col = cur_col - 80 + 1;
    }
}

static void draw_editor(cairo_t *cr, Display *dpy, Window win) {
    int visible_lines = (win_h - TOOLBAR_H - STATUS_H) / LINE_H;
    ensure_cursor_visible(visible_lines);

    // Update window title
    char win_title[600];
    const char *fn = current_filepath[0] ? current_filepath : "Untitled";
    snprintf(win_title, sizeof(win_title), "%s%s - flx-pad", fn, is_dirty ? "*" : "");
    XStoreName(dpy, win, win_title);

    // 1. Background
    cairo_set_source_rgb(cr, 0.11, 0.13, 0.17);
    cairo_paint(cr);

    // 2. Toolbar
    cairo_set_source_rgb(cr, 0.16, 0.19, 0.25);
    cairo_rectangle(cr, 0, 0, win_w, TOOLBAR_H);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.24, 0.30, 0.40);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, TOOLBAR_H - 0.5);
    cairo_line_to(cr, win_w, TOOLBAR_H - 0.5);
    cairo_stroke(cr);

    // Toolbar Buttons
    const char *btn_labels[] = {"New", "Open", "Save", "Save As"};
    int btn_widths[] = {50, 54, 54, 70};
    int bx = 8;
    for (int i = 0; i < 4; i++) {
        cairo_set_source_rgb(cr, 0.22, 0.26, 0.34);
        cairo_rectangle(cr, bx, 5, btn_widths[i], TOOLBAR_H - 10);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 0.35, 0.42, 0.55);
        cairo_rectangle(cr, bx + 0.5, 5.5, btn_widths[i] - 1.0, TOOLBAR_H - 11.0);
        cairo_stroke(cr);

        cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11.0);
        cairo_set_source_rgb(cr, 0.90, 0.93, 0.98);
        cairo_move_to(cr, bx + 10, 21);
        cairo_show_text(cr, btn_labels[i]);

        bx += btn_widths[i] + 6;
    }

    // Current File Label in Toolbar
    cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11.0);
    cairo_set_source_rgb(cr, 0.65, 0.72, 0.82);
    char path_disp[256];
    snprintf(path_disp, sizeof(path_disp), "File: %s%s", fn, is_dirty ? " (modified)" : "");
    cairo_move_to(cr, bx + 12, 21);
    cairo_show_text(cr, path_disp);

    // 3. Gutter Background
    cairo_set_source_rgb(cr, 0.14, 0.16, 0.21);
    cairo_rectangle(cr, 0, TOOLBAR_H, GUTTER_W, win_h - TOOLBAR_H - STATUS_H);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.22, 0.26, 0.34);
    cairo_move_to(cr, GUTTER_W - 0.5, TOOLBAR_H);
    cairo_line_to(cr, GUTTER_W - 0.5, win_h - STATUS_H);
    cairo_stroke(cr);

    // 4. Text Lines & Line Numbers
    cairo_select_font_face(cr, "DejaVu Sans Mono", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, FONT_SIZE);

    int editor_y = TOOLBAR_H;
    for (int i = 0; i < visible_lines; i++) {
        int lidx = scroll_line + i;
        if (lidx >= buf.count) break;

        int line_y = editor_y + i * LINE_H;

        // Current line highlight
        if (lidx == cur_line) {
            cairo_set_source_rgba(cr, 0.25, 0.45, 0.75, 0.15);
            cairo_rectangle(cr, GUTTER_W, line_y, win_w - GUTTER_W, LINE_H);
            cairo_fill(cr);
        }

        // Line number in gutter
        char num_str[16];
        snprintf(num_str, sizeof(num_str), "%d", lidx + 1);
        cairo_text_extents_t te;
        cairo_text_extents(cr, num_str, &te);
        cairo_set_source_rgb(cr, (lidx == cur_line) ? 0.85 : 0.42, (lidx == cur_line) ? 0.90 : 0.48, (lidx == cur_line) ? 1.0 : 0.58);
        cairo_move_to(cr, GUTTER_W - 8 - te.width, line_y + 13);
        cairo_show_text(cr, num_str);

        // Text content
        Line *l = &buf.lines[lidx];
        cairo_set_source_rgb(cr, 0.88, 0.92, 0.96);
        cairo_move_to(cr, GUTTER_W + 8, line_y + 13);

        const char *render_text = l->chars;
        if (scroll_col > 0 && scroll_col < l->len) {
            render_text = l->chars + scroll_col;
        } else if (scroll_col >= l->len) {
            render_text = "";
        }
        cairo_show_text(cr, render_text);

        // Cursor
        if (lidx == cur_line) {
            int cx = GUTTER_W + 8;
            if (cur_col > 0) {
                char prefix[512];
                int plen = cur_col - scroll_col;
                if (plen < 0) plen = 0;
                if (plen > (int)sizeof(prefix) - 1) plen = sizeof(prefix) - 1;
                if (scroll_col < l->len) {
                    strncpy(prefix, l->chars + scroll_col, plen);
                } else {
                    prefix[0] = '\0';
                }
                prefix[plen] = '\0';
                cairo_text_extents_t cte;
                cairo_text_extents(cr, prefix, &cte);
                cx += cte.x_advance;
            }

            cairo_set_source_rgb(cr, 0.40, 0.85, 1.0); // Glowing cyan cursor
            cairo_rectangle(cr, cx, line_y + 2, 2, LINE_H - 4);
            cairo_fill(cr);
        }
    }

    // 5. Status Bar
    int sb_y = win_h - STATUS_H;
    cairo_set_source_rgb(cr, 0.14, 0.17, 0.22);
    cairo_rectangle(cr, 0, sb_y, win_w, STATUS_H);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.22, 0.26, 0.35);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, sb_y + 0.5);
    cairo_line_to(cr, win_w, sb_y + 0.5);
    cairo_stroke(cr);

    // Status message
    cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.5);
    cairo_set_source_rgb(cr, 0.80, 0.85, 0.90);
    cairo_move_to(cr, 10, sb_y + 16);
    cairo_show_text(cr, status_msg);

    // Position info (right side)
    char pos_info[64];
    snprintf(pos_info, sizeof(pos_info), "Ln %d, Col %d   Lines: %d", cur_line + 1, cur_col + 1, buf.count);
    cairo_text_extents_t pte;
    cairo_text_extents(cr, pos_info, &pte);
    cairo_move_to(cr, win_w - pte.width - 14, sb_y + 16);
    cairo_set_source_rgb(cr, 0.45, 0.80, 0.98);
    cairo_show_text(cr, pos_info);

    // 6. Modal Prompt (if Open or Save As is active)
    if (prompt_mode != PROMPT_NONE) {
        int pw = 460;
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
        if (prompt_mode == PROMPT_OPEN) {
            cairo_show_text(cr, "Open File (Enter = OK, Esc = Cancel):");
        } else {
            cairo_show_text(cr, "Save As (Enter = OK, Esc = Cancel):");
        }

        // Input box
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
        cairo_show_text(cr, prompt_input);

        // Input cursor
        cairo_text_extents_t ite;
        cairo_text_extents(cr, prompt_input, &ite);
        cairo_set_source_rgb(cr, 0.40, 0.85, 1.0);
        cairo_rectangle(cr, px + 22 + ite.x_advance, py + 43, 2, 18);
        cairo_fill(cr);
    }
}

static void handle_typing(KeySym sym, char *text) {
    Line *l = &buf.lines[cur_line];

    if (sym == XK_Return) {
        buf_insert_line(cur_line + 1);
        Line *nl = &buf.lines[cur_line + 1];
        if (cur_col < l->len) {
            int rem = l->len - cur_col;
            if (rem + 1 > nl->cap) {
                nl->cap = rem + 16;
                nl->chars = realloc(nl->chars, nl->cap);
            }
            memcpy(nl->chars, l->chars + cur_col, rem);
            nl->len = rem;
            nl->chars[rem] = '\0';
            l->len = cur_col;
            l->chars[l->len] = '\0';
        }
        cur_line++;
        cur_col = 0;
        is_dirty = 1;
    } else if (sym == XK_BackSpace) {
        if (cur_col > 0) {
            line_delete_char(l, cur_col - 1);
            cur_col--;
            is_dirty = 1;
        } else if (cur_line > 0) {
            Line *prev = &buf.lines[cur_line - 1];
            int prev_len = prev->len;
            if (prev_len + l->len + 1 > prev->cap) {
                prev->cap = prev_len + l->len + 16;
                prev->chars = realloc(prev->chars, prev->cap);
            }
            memcpy(prev->chars + prev_len, l->chars, l->len);
            prev->len += l->len;
            prev->chars[prev->len] = '\0';
            buf_remove_line(cur_line);
            cur_line--;
            cur_col = prev_len;
            is_dirty = 1;
        }
    } else if (sym == XK_Delete) {
        if (cur_col < l->len) {
            line_delete_char(l, cur_col);
            is_dirty = 1;
        } else if (cur_line + 1 < buf.count) {
            Line *next = &buf.lines[cur_line + 1];
            if (l->len + next->len + 1 > l->cap) {
                l->cap = l->len + next->len + 16;
                l->chars = realloc(l->chars, l->cap);
            }
            memcpy(l->chars + l->len, next->chars, next->len);
            l->len += next->len;
            l->chars[l->len] = '\0';
            buf_remove_line(cur_line + 1);
            is_dirty = 1;
        }
    } else if (sym == XK_Tab) {
        for (int i = 0; i < 4; i++) {
            line_insert_char(l, cur_col++, ' ');
        }
        is_dirty = 1;
    } else if (text && text[0] >= 32) {
        line_insert_char(l, cur_col, text[0]);
        cur_col++;
        is_dirty = 1;
    }
}

int main(int argc, char **argv) {
    buf_init();

    if (argc > 1 && argv[1][0] != '\0') {
        buf_load_file(argv[1]);
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "flx-pad: Cannot open X display\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    Visual *vis = DefaultVisual(dpy, screen);
    int depth = DefaultDepth(dpy, screen);

    XSetWindowAttributes swa;
    swa.background_pixel = BlackPixel(dpy, screen);
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask;

    Window win = XCreateWindow(dpy, root, 100, 80, win_w, win_h, 0,
                               depth, InputOutput, vis,
                               CWBackPixel | CWEventMask, &swa);

    XStoreName(dpy, win, "flx-pad");
    XClassHint ch = {"flx-pad", "FreeLinX"};
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
            draw_editor(cr, dpy, win);
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

                draw_editor(cr, dpy, win);
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

            if (prompt_mode != PROMPT_NONE) {
                // Clicking outside prompt cancels it
                prompt_mode = PROMPT_NONE;
                draw_editor(cr, dpy, win);
                XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
                XFlush(dpy);
                continue;
            }

            if (my < TOOLBAR_H) {
                // Toolbar click
                if (mx >= 8 && mx < 8 + 50) {
                    // New
                    buf_clear();
                    snprintf(status_msg, sizeof(status_msg), "New document");
                } else if (mx >= 64 && mx < 64 + 54) {
                    // Open
                    prompt_mode = PROMPT_OPEN;
                    prompt_input[0] = '\0';
                    prompt_cur = 0;
                } else if (mx >= 124 && mx < 124 + 54) {
                    // Save
                    if (current_filepath[0]) {
                        buf_save_file(current_filepath);
                    } else {
                        prompt_mode = PROMPT_SAVE_AS;
                        prompt_input[0] = '\0';
                        prompt_cur = 0;
                    }
                } else if (mx >= 184 && mx < 184 + 70) {
                    // Save As
                    prompt_mode = PROMPT_SAVE_AS;
                    strncpy(prompt_input, current_filepath, sizeof(prompt_input) - 1);
                    prompt_cur = strlen(prompt_input);
                }
            } else if (ev.xbutton.button == 4) { // Scroll Up
                if (scroll_line > 0) scroll_line -= 3;
                if (scroll_line < 0) scroll_line = 0;
            } else if (ev.xbutton.button == 5) { // Scroll Down
                if (scroll_line + 3 < buf.count) scroll_line += 3;
            } else if (my >= TOOLBAR_H && my < win_h - STATUS_H) {
                // Click to place cursor
                int click_line = scroll_line + (my - TOOLBAR_H) / LINE_H;
                if (click_line >= 0 && click_line < buf.count) {
                    cur_line = click_line;
                    cur_col = 0;
                    Line *l = &buf.lines[cur_line];
                    int rel_x = mx - GUTTER_W - 8;
                    if (rel_x > 0) {
                        cur_col = rel_x / 8; // Mono character width approx
                        if (cur_col > l->len) cur_col = l->len;
                    }
                }
            }

            draw_editor(cr, dpy, win);
            XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
            XFlush(dpy);
        } else if (ev.type == KeyPress) {
            char kbuf[32];
            KeySym ksym;
            int nchars = XLookupString(&ev.xkey, kbuf, sizeof(kbuf) - 1, &ksym, NULL);
            kbuf[nchars] = '\0';

            int ctrl = (ev.xkey.state & ControlMask) != 0;

            if (prompt_mode != PROMPT_NONE) {
                if (ksym == XK_Escape) {
                    prompt_mode = PROMPT_NONE;
                } else if (ksym == XK_Return) {
                    if (prompt_mode == PROMPT_OPEN) {
                        if (prompt_input[0]) {
                            if (!buf_load_file(prompt_input)) {
                                snprintf(status_msg, sizeof(status_msg), "Failed to open '%s'", prompt_input);
                            }
                        }
                    } else if (prompt_mode == PROMPT_SAVE_AS) {
                        if (prompt_input[0]) {
                            buf_save_file(prompt_input);
                        }
                    }
                    prompt_mode = PROMPT_NONE;
                } else if (ksym == XK_BackSpace) {
                    if (prompt_cur > 0) {
                        prompt_input[--prompt_cur] = '\0';
                    }
                } else if (nchars > 0 && kbuf[0] >= 32) {
                    if (prompt_cur + 1 < (int)sizeof(prompt_input) - 1) {
                        prompt_input[prompt_cur++] = kbuf[0];
                        prompt_input[prompt_cur] = '\0';
                    }
                }
            } else if (ctrl) {
                if (ksym == XK_s || ksym == XK_S) {
                    if (current_filepath[0]) {
                        buf_save_file(current_filepath);
                    } else {
                        prompt_mode = PROMPT_SAVE_AS;
                        prompt_input[0] = '\0';
                        prompt_cur = 0;
                    }
                } else if (ksym == XK_o || ksym == XK_O) {
                    prompt_mode = PROMPT_OPEN;
                    prompt_input[0] = '\0';
                    prompt_cur = 0;
                } else if (ksym == XK_n || ksym == XK_N) {
                    buf_clear();
                    snprintf(status_msg, sizeof(status_msg), "New document");
                } else if (ksym == XK_q || ksym == XK_Q) {
                    running = 0;
                }
            } else {
                if (ksym == XK_Up) {
                    if (cur_line > 0) {
                        cur_line--;
                        if (cur_col > buf.lines[cur_line].len) {
                            cur_col = buf.lines[cur_line].len;
                        }
                    }
                } else if (ksym == XK_Down) {
                    if (cur_line + 1 < buf.count) {
                        cur_line++;
                        if (cur_col > buf.lines[cur_line].len) {
                            cur_col = buf.lines[cur_line].len;
                        }
                    }
                } else if (ksym == XK_Left) {
                    if (cur_col > 0) {
                        cur_col--;
                    } else if (cur_line > 0) {
                        cur_line--;
                        cur_col = buf.lines[cur_line].len;
                    }
                } else if (ksym == XK_Right) {
                    if (cur_col < buf.lines[cur_line].len) {
                        cur_col++;
                    } else if (cur_line + 1 < buf.count) {
                        cur_line++;
                        cur_col = 0;
                    }
                } else if (ksym == XK_Home) {
                    cur_col = 0;
                } else if (ksym == XK_End) {
                    cur_col = buf.lines[cur_line].len;
                } else if (ksym == XK_Page_Up) {
                    int vlines = (win_h - TOOLBAR_H - STATUS_H) / LINE_H;
                    cur_line -= vlines;
                    if (cur_line < 0) cur_line = 0;
                } else if (ksym == XK_Page_Down) {
                    int vlines = (win_h - TOOLBAR_H - STATUS_H) / LINE_H;
                    cur_line += vlines;
                    if (cur_line >= buf.count) cur_line = buf.count - 1;
                } else {
                    handle_typing(ksym, kbuf);
                }
            }

            draw_editor(cr, dpy, win);
            XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
            XFlush(dpy);
        }
    }

    buf_free();
    cairo_destroy(cr);
    cairo_surface_destroy(cs);
    XFreePixmap(dpy, backbuffer);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
