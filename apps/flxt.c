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

/* Classic 1990s Motif / CDE UI Dimensions */
#define MENUBAR_H   24
#define STATUS_H    22
#define LINE_H      18
#define FONT_SIZE   12.0
#define GUTTER_W    48

/* 1990s Motif & 16-Color ANSI Palette */
#define MOTIF_GRAY      0.753, 0.753, 0.753  /* #c0c0c0 Classic 90s Gray */
#define MOTIF_LIGHT     1.000, 1.000, 1.000  /* #ffffff 3D Highlight */
#define MOTIF_SHADOW    0.502, 0.502, 0.502  /* #808080 3D Shadow */
#define MOTIF_DARK      0.000, 0.000, 0.000  /* #000000 3D Outer Border */
#define MOTIF_TITLE_BG  0.000, 0.000, 0.502  /* #000080 Classic 90s Title Navy */
#define MOTIF_SELECT    0.000, 0.000, 0.502  /* Menu selection navy */

#define EDIT_BG         0.000, 0.000, 0.000  /* Pure 90s X11 Black */
#define EDIT_FG         1.000, 1.000, 1.000  /* Crisp White text */
#define GUTTER_BG       0.080, 0.080, 0.080  /* Deep dark gutter */
#define GUTTER_FG       0.500, 0.500, 0.500  /* Line number gray */

/* 90s Syntax Colors (Classic ANSI) */
#define SYN_KEYWORD     1.000, 1.000, 0.000  /* Yellow */
#define SYN_TYPE        0.000, 1.000, 1.000  /* Cyan */
#define SYN_STRING      0.000, 1.000, 0.000  /* Bright Green */
#define SYN_COMMENT     0.500, 0.500, 0.500  /* Gray */
#define SYN_NUMBER      1.000, 0.333, 0.333  /* Red */
#define SYN_PREPROC     1.000, 0.000, 1.000  /* Magenta */

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

static int win_w = 780;
static int win_h = 540;
static double char_w = 7.5;
static char status_msg[256] = "Ready";

/* Dialog Modes */
typedef enum {
    DLG_NONE = 0,
    DLG_OPEN,
    DLG_SAVE_AS,
    DLG_FIND,
    DLG_ABOUT
} DialogMode;

static DialogMode dlg_mode = DLG_NONE;
static char dlg_input[512] = "";
static int dlg_cur = 0;

/* Menu State */
static int active_menu = -1; /* -1 = None, 0 = File, 1 = Edit, 2 = Search, 3 = Help */

/* Undo Buffer */
typedef struct {
    char *text;
    int cur_line;
    int cur_col;
} UndoSnapshot;

#define MAX_UNDO 32
static UndoSnapshot undo_stack[MAX_UNDO];
static int undo_count = 0;

/* 3D Bevel Renderers */
static void draw_bevel_raised(cairo_t *cr, double x, double y, double w, double h) {
    /* Top & Left highlight */
    cairo_set_source_rgb(cr, MOTIF_LIGHT);
    cairo_rectangle(cr, x, y, w, 1);
    cairo_rectangle(cr, x, y, 1, h);
    cairo_fill(cr);

    /* Bottom & Right shadow */
    cairo_set_source_rgb(cr, MOTIF_SHADOW);
    cairo_rectangle(cr, x, y + h - 1, w, 1);
    cairo_rectangle(cr, x + w - 1, y, 1, h);
    cairo_fill(cr);
}

static void draw_bevel_sunken(cairo_t *cr, double x, double y, double w, double h) {
    /* Top & Left shadow */
    cairo_set_source_rgb(cr, MOTIF_SHADOW);
    cairo_rectangle(cr, x, y, w, 1);
    cairo_rectangle(cr, x, y, 1, h);
    cairo_fill(cr);

    /* Bottom & Right highlight */
    cairo_set_source_rgb(cr, MOTIF_LIGHT);
    cairo_rectangle(cr, x, y + h - 1, w, 1);
    cairo_rectangle(cr, x + w - 1, y, 1, h);
    cairo_fill(cr);
}

static void draw_panel_sunken_2px(cairo_t *cr, double x, double y, double w, double h) {
    /* Outer shadow */
    cairo_set_source_rgb(cr, MOTIF_SHADOW);
    cairo_rectangle(cr, x, y, w, 1);
    cairo_rectangle(cr, x, y, 1, h);
    cairo_fill(cr);
    /* Inner shadow */
    cairo_set_source_rgb(cr, MOTIF_DARK);
    cairo_rectangle(cr, x + 1, y + 1, w - 2, 1);
    cairo_rectangle(cr, x + 1, y + 1, 1, h - 2);
    cairo_fill(cr);
    /* Outer highlight */
    cairo_set_source_rgb(cr, MOTIF_LIGHT);
    cairo_rectangle(cr, x, y + h - 1, w, 1);
    cairo_rectangle(cr, x + w - 1, y, 1, h);
    cairo_fill(cr);
}

/* Buffer Management */
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
    for (int i = 0; i < buf.count; i++) free(buf.lines[i].chars);
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
    snprintf(status_msg, sizeof(status_msg), "New buffer");
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
    if (buf.count == 0) buf_init();
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
            if (nread > 0 && line_buf[nread - 1] == '\r') nread--;
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

    if (buf.count == 0) buf_init();

    strncpy(current_filepath, path, sizeof(current_filepath) - 1);
    current_filepath[sizeof(current_filepath) - 1] = '\0';
    cur_line = 0;
    cur_col = 0;
    scroll_line = 0;
    scroll_col = 0;
    is_dirty = 0;
    snprintf(status_msg, sizeof(status_msg), "Opened %s (%d lines)", path, buf.count);
    return 1;
}

static int buf_save_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(status_msg, sizeof(status_msg), "Error: Cannot write to %s", path);
        return 0;
    }
    for (int i = 0; i < buf.count; i++) {
        fputs(buf.lines[i].chars, f);
        fputc('\n', f);
    }
    fclose(f);
    strncpy(current_filepath, path, sizeof(current_filepath) - 1);
    is_dirty = 0;
    snprintf(status_msg, sizeof(status_msg), "Wrote %s (%d lines)", path, buf.count);
    return 1;
}

/* Undo Implementation */
static void undo_push(void) {
    if (undo_count >= MAX_UNDO) {
        free(undo_stack[0].text);
        memmove(&undo_stack[0], &undo_stack[1], sizeof(UndoSnapshot) * (MAX_UNDO - 1));
        undo_count--;
    }
    size_t total_len = 0;
    for (int i = 0; i < buf.count; i++) total_len += buf.lines[i].len + 1;
    char *s = malloc(total_len + 1);
    if (!s) return;
    char *p = s;
    for (int i = 0; i < buf.count; i++) {
        memcpy(p, buf.lines[i].chars, buf.lines[i].len);
        p += buf.lines[i].len;
        *p++ = '\n';
    }
    *p = '\0';
    undo_stack[undo_count].text = s;
    undo_stack[undo_count].cur_line = cur_line;
    undo_stack[undo_count].cur_col = cur_col;
    undo_count++;
}

static void undo_pop(void) {
    if (undo_count <= 0) {
        snprintf(status_msg, sizeof(status_msg), "Already at oldest change");
        return;
    }
    undo_count--;
    char *s = undo_stack[undo_count].text;
    cur_line = undo_stack[undo_count].cur_line;
    cur_col = undo_stack[undo_count].cur_col;

    buf_free();
    buf.cap = 64;
    buf.count = 0;
    buf.lines = malloc(sizeof(Line) * buf.cap);

    char *start = s;
    while (*start) {
        char *nl = strchr(start, '\n');
        size_t len = nl ? (size_t)(nl - start) : strlen(start);
        if (buf.count >= buf.cap) {
            buf.cap *= 2;
            buf.lines = realloc(buf.lines, sizeof(Line) * buf.cap);
        }
        Line *l = &buf.lines[buf.count];
        l->cap = len + 16;
        l->len = len;
        l->chars = malloc(l->cap);
        memcpy(l->chars, start, len);
        l->chars[len] = '\0';
        buf.count++;
        if (!nl) break;
        start = nl + 1;
    }
    free(s);
    if (buf.count == 0) buf_init();
    if (cur_line >= buf.count) cur_line = buf.count - 1;
    if (cur_col > buf.lines[cur_line].len) cur_col = buf.lines[cur_line].len;
    is_dirty = 1;
    snprintf(status_msg, sizeof(status_msg), "Undo applied");
}

/* 90s ANSI Syntax Detection */
static int is_c_file(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    return (!strcmp(dot, ".c") || !strcmp(dot, ".h") || !strcmp(dot, ".cpp") || !strcmp(dot, ".cc"));
}

static int is_c_keyword(const char *w, int len) {
    static const char *kw[] = {
        "auto", "break", "case", "char", "const", "continue", "default", "do",
        "double", "else", "enum", "extern", "float", "for", "goto", "if",
        "inline", "int", "long", "register", "restrict", "return", "short",
        "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
        "unsigned", "void", "volatile", "while", "bool", "true", "false", "NULL",
        "size_t", "ssize_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", NULL
    };
    for (int i = 0; kw[i]; i++) {
        if ((int)strlen(kw[i]) == len && !strncmp(kw[i], w, len)) return 1;
    }
    return 0;
}

static void ensure_cursor_visible(int visible_lines) {
    if (cur_line < scroll_line) scroll_line = cur_line;
    if (cur_line >= scroll_line + visible_lines) scroll_line = cur_line - visible_lines + 1;
    if (cur_col < scroll_col) scroll_col = cur_col;
    if (cur_col >= scroll_col + 80) scroll_col = cur_col - 80 + 1;
}

/* Render Line with Classic 1990s Syntax Coloring */
static void draw_syntax_line(cairo_t *cr, const char *s, int len, int line_y, int is_c) {
    int col = 0;
    int i = 0;

    cairo_select_font_face(cr, "DejaVu Sans Mono", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, FONT_SIZE);

    while (i < len) {
        if (col < scroll_col) {
            i++;
            col++;
            continue;
        }

        double rx = GUTTER_W + 6 + (col - scroll_col) * char_w;
        double ry = line_y + 13;

        /* Comment */
        if (is_c && s[i] == '/' && i + 1 < len && s[i + 1] == '/') {
            cairo_set_source_rgb(cr, SYN_COMMENT);
            cairo_move_to(cr, rx, ry);
            cairo_show_text(cr, s + i);
            break;
        }
        if (!is_c && s[i] == '#') {
            cairo_set_source_rgb(cr, SYN_COMMENT);
            cairo_move_to(cr, rx, ry);
            cairo_show_text(cr, s + i);
            break;
        }

        /* Preprocessor */
        if (is_c && s[i] == '#' && (i == 0 || isspace(s[i - 1]))) {
            int start = i;
            while (i < len && !isspace(s[i])) i++;
            int tlen = i - start;
            char token[64];
            if (tlen > 63) tlen = 63;
            strncpy(token, s + start, tlen);
            token[tlen] = '\0';
            cairo_set_source_rgb(cr, SYN_PREPROC);
            cairo_move_to(cr, rx, ry);
            cairo_show_text(cr, token);
            col += tlen;
            continue;
        }

        /* Strings */
        if (s[i] == '"' || s[i] == '\'') {
            char quote = s[i];
            int start = i++;
            while (i < len && s[i] != quote) {
                if (s[i] == '\\' && i + 1 < len) i++;
                i++;
            }
            if (i < len && s[i] == quote) i++;
            int slen = i - start;
            char *tmp = malloc(slen + 1);
            memcpy(tmp, s + start, slen);
            tmp[slen] = '\0';
            cairo_set_source_rgb(cr, SYN_STRING);
            cairo_move_to(cr, rx, ry);
            cairo_show_text(cr, tmp);
            free(tmp);
            col += slen;
            continue;
        }

        /* Numbers */
        if (isdigit(s[i]) && (i == 0 || !isalnum(s[i - 1]))) {
            int start = i;
            while (i < len && (isalnum(s[i]) || s[i] == '.' || s[i] == 'x' || s[i] == 'X')) i++;
            int nlen = i - start;
            char *tmp = malloc(nlen + 1);
            memcpy(tmp, s + start, nlen);
            tmp[nlen] = '\0';
            cairo_set_source_rgb(cr, SYN_NUMBER);
            cairo_move_to(cr, rx, ry);
            cairo_show_text(cr, tmp);
            free(tmp);
            col += nlen;
            continue;
        }

        /* Identifiers */
        if (isalpha(s[i]) || s[i] == '_') {
            int start = i;
            while (i < len && (isalnum(s[i]) || s[i] == '_')) i++;
            int wlen = i - start;
            char *tmp = malloc(wlen + 1);
            memcpy(tmp, s + start, wlen);
            tmp[wlen] = '\0';

            if (is_c && is_c_keyword(tmp, wlen)) {
                cairo_set_source_rgb(cr, SYN_KEYWORD);
            } else if (i < len && s[i] == '(') {
                cairo_set_source_rgb(cr, SYN_TYPE);
            } else {
                cairo_set_source_rgb(cr, EDIT_FG);
            }
            cairo_move_to(cr, rx, ry);
            cairo_show_text(cr, tmp);
            free(tmp);
            col += wlen;
            continue;
        }

        /* Punctuation */
        char ch[2] = { s[i], '\0' };
        cairo_set_source_rgb(cr, EDIT_FG);
        cairo_move_to(cr, rx, ry);
        cairo_show_text(cr, ch);
        i++;
        col++;
    }
}

/* Main Draw Routine */
static void draw_editor(cairo_t *cr, Display *dpy, Window win) {
    int visible_lines = (win_h - MENUBAR_H - STATUS_H - 4) / LINE_H;
    ensure_cursor_visible(visible_lines);

    const char *fn = current_filepath[0] ? current_filepath : "Untitled";
    char win_title[600];
    snprintf(win_title, sizeof(win_title), "flxt - %s%s", fn, is_dirty ? " [Modified]" : "");
    XStoreName(dpy, win, win_title);

    /* 1. Whole Window Base Background (Motif Gray) */
    cairo_set_source_rgb(cr, MOTIF_GRAY);
    cairo_paint(cr);

    /* 2. Classic 1990s Menu Bar */
    cairo_rectangle(cr, 0, 0, win_w, MENUBAR_H);
    cairo_fill(cr);
    draw_bevel_raised(cr, 0, 0, win_w, MENUBAR_H);

    static const char *menus[] = { "File", "Edit", "Search", "Help" };
    static int menu_x[] = { 6, 60, 114, 186 };
    static int menu_w_arr[] = { 50, 50, 68, 50 };

    cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11.0);

    for (int i = 0; i < 4; i++) {
        if (active_menu == i) {
            draw_bevel_sunken(cr, menu_x[i], 2, menu_w_arr[i], MENUBAR_H - 4);
            cairo_set_source_rgb(cr, MOTIF_SELECT);
            cairo_rectangle(cr, menu_x[i] + 1, 3, menu_w_arr[i] - 2, MENUBAR_H - 6);
            cairo_fill(cr);
            cairo_set_source_rgb(cr, MOTIF_LIGHT);
        } else {
            cairo_set_source_rgb(cr, MOTIF_DARK);
        }
        cairo_move_to(cr, menu_x[i] + 12, 16);
        cairo_show_text(cr, menus[i]);
    }

    /* Right label in menubar */
    cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.5);
    cairo_set_source_rgb(cr, MOTIF_SHADOW);
    const char *ver = "flxt v1.0 (X11/Motif)";
    cairo_text_extents_t vte;
    cairo_text_extents(cr, ver, &vte);
    cairo_move_to(cr, win_w - vte.width - 10, 16);
    cairo_show_text(cr, ver);

    /* 3. Sunken Text Area Frame */
    int edit_y = MENUBAR_H + 2;
    int edit_h = win_h - MENUBAR_H - STATUS_H - 4;
    draw_panel_sunken_2px(cr, 2, edit_y, win_w - 4, edit_h);

    /* Editor Background */
    cairo_set_source_rgb(cr, EDIT_BG);
    cairo_rectangle(cr, 4, edit_y + 2, win_w - 8, edit_h - 4);
    cairo_fill(cr);

    /* Gutter Background */
    cairo_set_source_rgb(cr, GUTTER_BG);
    cairo_rectangle(cr, 4, edit_y + 2, GUTTER_W, edit_h - 4);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
    cairo_rectangle(cr, 4 + GUTTER_W - 1, edit_y + 2, 1, edit_h - 4);
    cairo_fill(cr);

    /* 4. Text Lines */
    int is_c = is_c_file(current_filepath);

    for (int i = 0; i < visible_lines; i++) {
        int lidx = scroll_line + i;
        if (lidx >= buf.count) break;
        int line_y = edit_y + 4 + i * LINE_H;

        /* Current line subtle background highlight */
        if (lidx == cur_line) {
            cairo_set_source_rgb(cr, 0.12, 0.14, 0.18);
            cairo_rectangle(cr, 4 + GUTTER_W, line_y, win_w - 8 - GUTTER_W, LINE_H);
            cairo_fill(cr);
        }

        /* Line number */
        char num_str[16];
        snprintf(num_str, sizeof(num_str), "%d", lidx + 1);
        cairo_select_font_face(cr, "DejaVu Sans Mono", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, FONT_SIZE);
        cairo_text_extents_t nte;
        cairo_text_extents(cr, num_str, &nte);
        cairo_set_source_rgb(cr, (lidx == cur_line) ? 0.90 : 0.45, (lidx == cur_line) ? 0.90 : 0.45, (lidx == cur_line) ? 0.90 : 0.45);
        cairo_move_to(cr, 4 + GUTTER_W - 6 - nte.width, line_y + 13);
        cairo_show_text(cr, num_str);

        /* Line content */
        Line *l = &buf.lines[lidx];
        draw_syntax_line(cr, l->chars, l->len, line_y, is_c);

        /* Cursor: Classic 90s Solid Block */
        if (lidx == cur_line) {
            double cx = 4 + GUTTER_W + 6 + (cur_col - scroll_col) * char_w;
            if (cx >= 4 + GUTTER_W && cx < win_w - 12) {
                cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
                cairo_rectangle(cr, cx, line_y + 1, char_w, LINE_H - 2);
                cairo_fill(cr);

                /* Draw char inside cursor in black */
                if (cur_col < l->len) {
                    char ch[2] = { l->chars[cur_col], '\0' };
                    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
                    cairo_move_to(cr, cx, line_y + 13);
                    cairo_show_text(cr, ch);
                }
            }
        }
    }

    /* 5. Classic 1990s Status Bar */
    int sb_y = win_h - STATUS_H - 2;

    /* Section 1: File & Status Message */
    int sec1_w = win_w - 290;
    draw_bevel_sunken(cr, 2, sb_y, sec1_w, STATUS_H);
    cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.5);
    cairo_set_source_rgb(cr, MOTIF_DARK);
    cairo_move_to(cr, 8, sb_y + 15);
    cairo_show_text(cr, status_msg);

    /* Section 2: Position */
    char pos_str[64];
    snprintf(pos_str, sizeof(pos_str), "Ln %d, Col %d", cur_line + 1, cur_col + 1);
    draw_bevel_sunken(cr, 2 + sec1_w + 2, sb_y, 110, STATUS_H);
    cairo_move_to(cr, 2 + sec1_w + 10, sb_y + 15);
    cairo_show_text(cr, pos_str);

    /* Section 3: Total Lines */
    char lines_str[64];
    snprintf(lines_str, sizeof(lines_str), "%d lines", buf.count);
    draw_bevel_sunken(cr, 2 + sec1_w + 116, sb_y, 90, STATUS_H);
    cairo_move_to(cr, 2 + sec1_w + 124, sb_y + 15);
    cairo_show_text(cr, lines_str);

    /* Section 4: Mode */
    draw_bevel_sunken(cr, 2 + sec1_w + 208, sb_y, 74, STATUS_H);
    cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_move_to(cr, 2 + sec1_w + 224, sb_y + 15);
    cairo_show_text(cr, "INS");

    /* 6. Active Dropdown Menu */
    if (active_menu >= 0) {
        int drop_x = menu_x[active_menu];
        int drop_y = MENUBAR_H;
        int drop_w = 170;
        int drop_h = 0;

        static const char *f_items[] = { "New          Ctrl+N", "Open...      Ctrl+O", "Save         Ctrl+S", "Save As...", "Exit         Ctrl+Q" };
        static const char *e_items[] = { "Undo         Ctrl+Z", "Cut Line     Ctrl+K", "Clear All" };
        static const char *s_items[] = { "Find...      Ctrl+F" };
        static const char *h_items[] = { "About flxt..." };

        const char **items = NULL;
        int item_cnt = 0;

        if (active_menu == 0) { items = f_items; item_cnt = 5; }
        else if (active_menu == 1) { items = e_items; item_cnt = 3; }
        else if (active_menu == 2) { items = s_items; item_cnt = 1; }
        else if (active_menu == 3) { items = h_items; item_cnt = 1; }

        drop_h = item_cnt * 22 + 4;

        /* Dropdown Background */
        cairo_set_source_rgb(cr, MOTIF_GRAY);
        cairo_rectangle(cr, drop_x, drop_y, drop_w, drop_h);
        cairo_fill(cr);
        draw_bevel_raised(cr, drop_x, drop_y, drop_w, drop_h);

        cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.0);
        for (int m = 0; m < item_cnt; m++) {
            cairo_set_source_rgb(cr, MOTIF_DARK);
            cairo_move_to(cr, drop_x + 12, drop_y + 16 + m * 22);
            cairo_show_text(cr, items[m]);
        }
    }

    /* 7. Classic 1990s Dialog Box */
    if (dlg_mode != DLG_NONE) {
        int dw = 420;
        int dh = (dlg_mode == DLG_ABOUT) ? 170 : 130;
        int dx = (win_w - dw) / 2;
        int dy = (win_h - dh) / 2;

        /* Window frame */
        cairo_set_source_rgb(cr, MOTIF_GRAY);
        cairo_rectangle(cr, dx, dy, dw, dh);
        cairo_fill(cr);
        draw_bevel_raised(cr, dx, dy, dw, dh);

        /* Title Bar */
        cairo_set_source_rgb(cr, MOTIF_TITLE_BG);
        cairo_rectangle(cr, dx + 3, dy + 3, dw - 6, 20);
        cairo_fill(cr);

        cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11.0);
        cairo_set_source_rgb(cr, MOTIF_LIGHT);
        cairo_move_to(cr, dx + 8, dy + 17);

        if (dlg_mode == DLG_OPEN) cairo_show_text(cr, "Open File");
        else if (dlg_mode == DLG_SAVE_AS) cairo_show_text(cr, "Save File As");
        else if (dlg_mode == DLG_FIND) cairo_show_text(cr, "Find Text");
        else if (dlg_mode == DLG_ABOUT) cairo_show_text(cr, "About flxt");

        if (dlg_mode == DLG_ABOUT) {
            cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_source_rgb(cr, MOTIF_DARK);
            cairo_move_to(cr, dx + 20, dy + 52);
            cairo_show_text(cr, "flxt - FreeLinX Text Editor");

            cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_move_to(cr, dx + 20, dy + 74);
            cairo_show_text(cr, "Version 1.0 (Motif/X11 Classic)");

            cairo_move_to(cr, dx + 20, dy + 94);
            cairo_show_text(cr, "Fast, lightweight Unix CDE/Motif text editor.");

            /* OK Button */
            int bx = dx + (dw - 70) / 2;
            int by = dy + 124;
            cairo_set_source_rgb(cr, MOTIF_GRAY);
            cairo_rectangle(cr, bx, by, 70, 24);
            cairo_fill(cr);
            draw_bevel_raised(cr, bx, by, 70, 24);

            cairo_set_source_rgb(cr, MOTIF_DARK);
            cairo_move_to(cr, bx + 24, by + 16);
            cairo_show_text(cr, "OK");
        } else {
            /* Input Box Label */
            cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 11.0);
            cairo_set_source_rgb(cr, MOTIF_DARK);
            cairo_move_to(cr, dx + 16, dy + 44);
            if (dlg_mode == DLG_OPEN) cairo_show_text(cr, "File Name:");
            else if (dlg_mode == DLG_SAVE_AS) cairo_show_text(cr, "Save As:");
            else if (dlg_mode == DLG_FIND) cairo_show_text(cr, "Search String:");

            /* Sunken Input Field */
            int fx = dx + 16;
            int fy = dy + 52;
            int fw = dw - 32;
            int fh = 24;
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            cairo_rectangle(cr, fx, fy, fw, fh);
            cairo_fill(cr);
            draw_bevel_sunken(cr, fx, fy, fw, fh);

            cairo_select_font_face(cr, "DejaVu Sans Mono", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 11.5);
            cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
            cairo_move_to(cr, fx + 6, fy + 16);
            cairo_show_text(cr, dlg_input);

            /* Input Cursor */
            cairo_text_extents_t dte;
            cairo_text_extents(cr, dlg_input, &dte);
            cairo_rectangle(cr, fx + 6 + dte.x_advance, fy + 3, 1, fh - 6);
            cairo_fill(cr);

            /* Buttons [OK] [Cancel] */
            int bx1 = dx + dw - 160;
            int bx2 = dx + dw - 80;
            int by = dy + 90;

            cairo_set_source_rgb(cr, MOTIF_GRAY);
            cairo_rectangle(cr, bx1, by, 70, 24);
            cairo_rectangle(cr, bx2, by, 70, 24);
            cairo_fill(cr);

            draw_bevel_raised(cr, bx1, by, 70, 24);
            draw_bevel_raised(cr, bx2, by, 70, 24);

            cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 11.0);
            cairo_set_source_rgb(cr, MOTIF_DARK);
            cairo_move_to(cr, bx1 + 24, by + 16);
            cairo_show_text(cr, "OK");
            cairo_move_to(cr, bx2 + 14, by + 16);
            cairo_show_text(cr, "Cancel");
        }
    }
}

/* Search Functionality */
static void do_find(const char *query) {
    if (!query || !query[0]) return;
    for (int l = cur_line; l < buf.count; l++) {
        int offset = (l == cur_line) ? (cur_col + 1) : 0;
        if (offset < buf.lines[l].len) {
            char *p = strstr(buf.lines[l].chars + offset, query);
            if (p) {
                cur_line = l;
                cur_col = p - buf.lines[l].chars;
                snprintf(status_msg, sizeof(status_msg), "Found match at line %d, col %d", cur_line + 1, cur_col + 1);
                return;
            }
        }
    }
    snprintf(status_msg, sizeof(status_msg), "String '%s' not found", query);
}

/* Typing & Editing Handler (Modeless Instant Editing) */
static void handle_editor_typing(KeySym sym, char *text) {
    Line *l = &buf.lines[cur_line];

    if (sym == XK_Return) {
        undo_push();
        int indent = 0;
        while (indent < l->len && l->chars[indent] == ' ') indent++;

        buf_insert_line(cur_line + 1);
        Line *nl = &buf.lines[cur_line + 1];

        int rem = (cur_col < l->len) ? (l->len - cur_col) : 0;
        int new_total = indent + rem;

        if (new_total + 16 > nl->cap) {
            nl->cap = new_total + 32;
            nl->chars = realloc(nl->chars, nl->cap);
        }

        if (indent > 0) memset(nl->chars, ' ', indent);
        if (rem > 0) memcpy(nl->chars + indent, l->chars + cur_col, rem);
        nl->len = new_total;
        nl->chars[new_total] = '\0';

        if (cur_col < l->len) {
            l->len = cur_col;
            l->chars[cur_col] = '\0';
        }

        cur_line++;
        cur_col = indent;
        is_dirty = 1;
    } else if (sym == XK_BackSpace) {
        if (cur_col > 0) {
            undo_push();
            line_delete_char(l, cur_col - 1);
            cur_col--;
            is_dirty = 1;
        } else if (cur_line > 0) {
            undo_push();
            Line *prev = &buf.lines[cur_line - 1];
            int prev_len = prev->len;
            if (prev_len + l->len + 1 > prev->cap) {
                prev->cap = prev_len + l->len + 32;
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
            undo_push();
            line_delete_char(l, cur_col);
            is_dirty = 1;
        } else if (cur_line + 1 < buf.count) {
            undo_push();
            Line *next = &buf.lines[cur_line + 1];
            if (l->len + next->len + 1 > l->cap) {
                l->cap = l->len + next->len + 32;
                l->chars = realloc(l->chars, l->cap);
            }
            memcpy(l->chars + l->len, next->chars, next->len);
            l->len += next->len;
            l->chars[l->len] = '\0';
            buf_remove_line(cur_line + 1);
            is_dirty = 1;
        }
    } else if (sym == XK_Tab) {
        undo_push();
        for (int i = 0; i < 4; i++) line_insert_char(l, cur_col++, ' ');
        is_dirty = 1;
    } else if (sym == XK_Left) {
        if (cur_col > 0) cur_col--;
        else if (cur_line > 0) { cur_line--; cur_col = buf.lines[cur_line].len; }
    } else if (sym == XK_Right) {
        if (cur_col < l->len) cur_col++;
        else if (cur_line + 1 < buf.count) { cur_line++; cur_col = 0; }
    } else if (sym == XK_Up) {
        if (cur_line > 0) {
            cur_line--;
            if (cur_col > buf.lines[cur_line].len) cur_col = buf.lines[cur_line].len;
        }
    } else if (sym == XK_Down) {
        if (cur_line + 1 < buf.count) {
            cur_line++;
            if (cur_col > buf.lines[cur_line].len) cur_col = buf.lines[cur_line].len;
        }
    } else if (sym == XK_Home) {
        cur_col = 0;
    } else if (sym == XK_End) {
        cur_col = l->len;
    } else if (sym == XK_Page_Up) {
        int vlines = (win_h - MENUBAR_H - STATUS_H - 4) / LINE_H;
        cur_line -= vlines;
        if (cur_line < 0) cur_line = 0;
    } else if (sym == XK_Page_Down) {
        int vlines = (win_h - MENUBAR_H - STATUS_H - 4) / LINE_H;
        cur_line += vlines;
        if (cur_line >= buf.count) cur_line = buf.count - 1;
    } else if (text && (unsigned char)text[0] >= 32) {
        undo_push();
        line_insert_char(l, cur_col++, text[0]);
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
        fprintf(stderr, "flxt: Cannot open X display\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    Visual *vis = DefaultVisual(dpy, screen);
    int depth = DefaultDepth(dpy, screen);

    XSetWindowAttributes swa;
    swa.background_pixel = BlackPixel(dpy, screen);
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask;

    Window win = XCreateWindow(dpy, root, 120, 90, win_w, win_h, 0,
                               depth, InputOutput, vis,
                               CWBackPixel | CWEventMask, &swa);

    XStoreName(dpy, win, "flxt");
    XClassHint ch = {"flxt", "FreeLinX"};
    XSetClassHint(dpy, win, &ch);

    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    Pixmap backbuffer = XCreatePixmap(dpy, win, win_w, win_h, depth);
    GC gc = XCreateGC(dpy, win, 0, NULL);
    cairo_surface_t *cs = cairo_xlib_surface_create(dpy, backbuffer, vis, win_w, win_h);
    cairo_t *cr = cairo_create(cs);

    cairo_select_font_face(cr, "DejaVu Sans Mono", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, FONT_SIZE);
    cairo_text_extents_t m_cte;
    cairo_text_extents(cr, "M", &m_cte);
    if (m_cte.x_advance > 2.0) char_w = m_cte.x_advance;

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
            if ((Atom)ev.xclient.data.l[0] == wm_delete) running = 0;
        } else if (ev.type == ButtonPress) {
            int mx = ev.xbutton.x;
            int my = ev.xbutton.y;

            if (dlg_mode != DLG_NONE) {
                int dw = 420;
                int dh = (dlg_mode == DLG_ABOUT) ? 170 : 130;
                int dx = (win_w - dw) / 2;
                int dy = (win_h - dh) / 2;

                if (dlg_mode == DLG_ABOUT) {
                    int bx = dx + (dw - 70) / 2;
                    int by = dy + 124;
                    if (mx >= bx && mx <= bx + 70 && my >= by && my <= by + 24) {
                        dlg_mode = DLG_NONE;
                    }
                } else {
                    int bx1 = dx + dw - 160;
                    int bx2 = dx + dw - 80;
                    int by = dy + 90;
                    if (mx >= bx1 && mx <= bx1 + 70 && my >= by && my <= by + 24) {
                        /* OK */
                        if (dlg_mode == DLG_OPEN && dlg_input[0]) {
                            if (!buf_load_file(dlg_input)) snprintf(status_msg, sizeof(status_msg), "Failed to open %s", dlg_input);
                        } else if (dlg_mode == DLG_SAVE_AS && dlg_input[0]) {
                            buf_save_file(dlg_input);
                        } else if (dlg_mode == DLG_FIND && dlg_input[0]) {
                            do_find(dlg_input);
                        }
                        dlg_mode = DLG_NONE;
                    } else if (mx >= bx2 && mx <= bx2 + 70 && my >= by && my <= by + 24) {
                        /* Cancel */
                        dlg_mode = DLG_NONE;
                    }
                }
            } else if (active_menu >= 0 && my >= MENUBAR_H && my <= MENUBAR_H + 115) {
                /* Click inside dropdown menu */
                int item_idx = (my - MENUBAR_H) / 22;
                if (active_menu == 0) {
                    if (item_idx == 0) buf_clear();
                    else if (item_idx == 1) { dlg_mode = DLG_OPEN; dlg_input[0] = '\0'; dlg_cur = 0; }
                    else if (item_idx == 2) {
                        if (current_filepath[0]) buf_save_file(current_filepath);
                        else { dlg_mode = DLG_SAVE_AS; dlg_input[0] = '\0'; dlg_cur = 0; }
                    } else if (item_idx == 3) {
                        dlg_mode = DLG_SAVE_AS;
                        strncpy(dlg_input, current_filepath, sizeof(dlg_input) - 1);
                        dlg_cur = strlen(dlg_input);
                    } else if (item_idx == 4) running = 0;
                } else if (active_menu == 1) {
                    if (item_idx == 0) undo_pop();
                    else if (item_idx == 1) {
                        undo_push();
                        buf_remove_line(cur_line);
                        if (cur_line >= buf.count) cur_line = buf.count - 1;
                        cur_col = 0;
                        is_dirty = 1;
                    } else if (item_idx == 2) buf_clear();
                } else if (active_menu == 2) {
                    if (item_idx == 0) { dlg_mode = DLG_FIND; dlg_input[0] = '\0'; dlg_cur = 0; }
                } else if (active_menu == 3) {
                    if (item_idx == 0) dlg_mode = DLG_ABOUT;
                }
                active_menu = -1;
            } else if (my < MENUBAR_H) {
                /* Click Menubar */
                if (mx >= 6 && mx <= 56) active_menu = (active_menu == 0) ? -1 : 0;
                else if (mx >= 60 && mx <= 110) active_menu = (active_menu == 1) ? -1 : 1;
                else if (mx >= 114 && mx <= 182) active_menu = (active_menu == 2) ? -1 : 2;
                else if (mx >= 186 && mx <= 236) active_menu = (active_menu == 3) ? -1 : 3;
                else active_menu = -1;
            } else if (ev.xbutton.button == 4) { /* Scroll Up */
                if (scroll_line > 0) scroll_line -= 3;
                if (scroll_line < 0) scroll_line = 0;
                active_menu = -1;
            } else if (ev.xbutton.button == 5) { /* Scroll Down */
                if (scroll_line + 3 < buf.count) scroll_line += 3;
                active_menu = -1;
            } else if (my >= MENUBAR_H && my < win_h - STATUS_H) {
                /* Click in editor text */
                active_menu = -1;
                int edit_y = MENUBAR_H + 2;
                int click_line = scroll_line + (my - edit_y - 4) / LINE_H;
                if (click_line >= 0 && click_line < buf.count) {
                    cur_line = click_line;
                    cur_col = 0;
                    Line *l = &buf.lines[cur_line];
                    int rel_x = mx - (4 + GUTTER_W + 6);
                    if (rel_x > 0) {
                        cur_col = rel_x / char_w;
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

            if (dlg_mode != DLG_NONE) {
                if (ksym == XK_Escape) {
                    dlg_mode = DLG_NONE;
                } else if (ksym == XK_Return) {
                    if (dlg_mode == DLG_OPEN && dlg_input[0]) {
                        if (!buf_load_file(dlg_input)) snprintf(status_msg, sizeof(status_msg), "Failed to open %s", dlg_input);
                    } else if (dlg_mode == DLG_SAVE_AS && dlg_input[0]) {
                        buf_save_file(dlg_input);
                    } else if (dlg_mode == DLG_FIND && dlg_input[0]) {
                        do_find(dlg_input);
                    }
                    dlg_mode = DLG_NONE;
                } else if (ksym == XK_BackSpace) {
                    if (dlg_cur > 0) dlg_input[--dlg_cur] = '\0';
                } else if (nchars > 0 && kbuf[0] >= 32) {
                    if (dlg_cur + 1 < (int)sizeof(dlg_input) - 1) {
                        dlg_input[dlg_cur++] = kbuf[0];
                        dlg_input[dlg_cur] = '\0';
                    }
                }
            } else if (ctrl) {
                if (ksym == XK_s || ksym == XK_S) {
                    if (current_filepath[0]) buf_save_file(current_filepath);
                    else { dlg_mode = DLG_SAVE_AS; dlg_input[0] = '\0'; dlg_cur = 0; }
                } else if (ksym == XK_o || ksym == XK_O) {
                    dlg_mode = DLG_OPEN;
                    dlg_input[0] = '\0';
                    dlg_cur = 0;
                } else if (ksym == XK_n || ksym == XK_N) {
                    buf_clear();
                } else if (ksym == XK_q || ksym == XK_Q) {
                    running = 0;
                } else if (ksym == XK_f || ksym == XK_F) {
                    dlg_mode = DLG_FIND;
                    dlg_input[0] = '\0';
                    dlg_cur = 0;
                } else if (ksym == XK_z || ksym == XK_Z) {
                    undo_pop();
                } else if (ksym == XK_k || ksym == XK_K) {
                    undo_push();
                    buf_remove_line(cur_line);
                    if (cur_line >= buf.count) cur_line = buf.count - 1;
                    cur_col = 0;
                    is_dirty = 1;
                }
            } else if (ksym == XK_Escape) {
                active_menu = -1;
            } else {
                handle_editor_typing(ksym, kbuf);
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
