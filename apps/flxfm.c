#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#define MAX_ITEMS 2048
#define WIN_DEFAULT_W 780
#define WIN_DEFAULT_H 520

#define TOP_BAR_H 34
#define PATH_BAR_H 26
#define HEADER_H 22
#define STATUS_BAR_H 22
#define ROW_H 22
#define SCROLL_W 14

typedef struct {
    char name[256];
    char size_str[32];
    char mode_str[12];
    char date_str[32];
    int is_dir;
    int is_exec;
    int is_link;
    off_t size;
    time_t mtime;
} FileItem;

typedef struct {
    int x, y, w, h;
    const char *label;
    int id;
} Button;

enum {
    BTN_ROOT = 1,
    BTN_HOME,
    BTN_UP,
    BTN_REFRESH,
    BTN_MKDIR,
    BTN_TOUCH,
    BTN_RENAME,
    BTN_DELETE,
    BTN_INFO,
    BTN_HIDDEN,
    BTN_TERM,
    BTN_COUNT
};

enum DialogMode {
    DIALOG_NONE = 0,
    DIALOG_MKDIR,
    DIALOG_TOUCH,
    DIALOG_RENAME,
    DIALOG_DELETE,
    DIALOG_INFO,
    DIALOG_GOTO
};

static Button buttons[] = {
    {0, 6, 48, 22, "/ Root",    BTN_ROOT},
    {0, 6, 52, 22, "~ Home",    BTN_HOME},
    {0, 6, 44, 22, ".. Up",     BTN_UP},
    {0, 6, 58, 22, "Reload",    BTN_REFRESH},
    {0, 6, 64, 22, "+ Folder",  BTN_MKDIR},
    {0, 6, 52, 22, "+ File",    BTN_TOUCH},
    {0, 6, 56, 22, "Rename",    BTN_RENAME},
    {0, 6, 56, 22, "Delete",    BTN_DELETE},
    {0, 6, 46, 22, "Info",      BTN_INFO},
    {0, 6, 56, 22, "Hidden",    BTN_HIDDEN},
    {0, 6, 52, 22, ">_ Term",   BTN_TERM},
};
#define NUM_BUTTONS (sizeof(buttons)/sizeof(buttons[0]))

static FileItem items[MAX_ITEMS];
static int item_count = 0;
static int dir_count = 0;
static int file_count = 0;
static int selected_idx = 0;
static int scroll_offset = 0;
static int show_hidden = 0;
static char current_path[1024] = "/root";
static char status_text[256] = "";

static struct timespec last_click_time = {0, 0};
static int last_clicked_idx = -1;

// Modal Dialog state
static int dialog_mode = DIALOG_NONE;
static char dialog_title[128] = "";
static char dialog_prompt[256] = "";
static char dialog_buf[512] = "";
static int dialog_cursor = 0;
static char dialog_info_lines[10][256];
static int dialog_info_line_count = 0;

static int dlg_btn1_x = 0, dlg_btn1_y = 0, dlg_btn1_w = 0, dlg_btn1_h = 0;
static int dlg_btn2_x = 0, dlg_btn2_y = 0, dlg_btn2_w = 0, dlg_btn2_h = 0;

// Forward declarations
static void load_directory(const char *path);
static void open_item(int idx);
static void update_status(void);
static void commit_dialog_action(void);

static void layout_buttons(void) {
    int cur_x = 8;
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        buttons[i].x = cur_x;
        buttons[i].y = 6;
        buttons[i].h = 22;
        cur_x += buttons[i].w + 4;
    }
}

static int compare_items(const void *a, const void *b) {
    const FileItem *ia = (const FileItem *)a;
    const FileItem *ib = (const FileItem *)b;
    if (ia->is_dir && !ib->is_dir) return -1;
    if (!ia->is_dir && ib->is_dir) return 1;
    return strcasecmp(ia->name, ib->name);
}

static void format_size(off_t sz, char *out, size_t out_len) {
    if (sz < 1024)
        snprintf(out, out_len, "%ld B", (long)sz);
    else if (sz < 1024 * 1024)
        snprintf(out, out_len, "%.1f KB", (double)sz / 1024.0);
    else if (sz < 1024 * 1024 * 1024)
        snprintf(out, out_len, "%.1f MB", (double)sz / (1024.0 * 1024.0));
    else
        snprintf(out, out_len, "%.2f GB", (double)sz / (1024.0 * 1024.0 * 1024.0));
}

static void format_mode(mode_t m, char *out) {
    out[0] = S_ISDIR(m) ? 'd' : (S_ISLNK(m) ? 'l' : '-');
    out[1] = (m & S_IRUSR) ? 'r' : '-';
    out[2] = (m & S_IWUSR) ? 'w' : '-';
    out[3] = (m & S_IXUSR) ? 'x' : '-';
    out[4] = (m & S_IRGRP) ? 'r' : '-';
    out[5] = (m & S_IWGRP) ? 'w' : '-';
    out[6] = (m & S_IXGRP) ? 'x' : '-';
    out[7] = (m & S_IROTH) ? 'r' : '-';
    out[8] = (m & S_IWOTH) ? 'w' : '-';
    out[9] = (m & S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

static void format_date(time_t mtime, char *out, size_t out_len) {
    struct tm *tm = localtime(&mtime);
    if (tm)
        strftime(out, out_len, "%Y-%m-%d %H:%M", tm);
    else
        snprintf(out, out_len, "-");
}

static void update_status(void) {
    struct statvfs sv;
    char free_str[64] = "";
    if (statvfs(current_path, &sv) == 0) {
        unsigned long long free_bytes = (unsigned long long)sv.f_bavail * sv.f_frsize;
        char sz[32];
        format_size((off_t)free_bytes, sz, sizeof(sz));
        snprintf(free_str, sizeof(free_str), " | Free: %s", sz);
    }

    if (selected_idx >= 0 && selected_idx < item_count) {
        snprintf(status_text, sizeof(status_text),
                 "%d items (%d dirs, %d files) | Selected: %s (%s)%s",
                 item_count, dir_count, file_count,
                 items[selected_idx].name,
                 items[selected_idx].is_dir ? "folder" : items[selected_idx].size_str,
                 free_str);
    } else {
        snprintf(status_text, sizeof(status_text),
                 "%d items (%d dirs, %d files)%s",
                 item_count, dir_count, file_count, free_str);
    }
}

static void load_directory(const char *path) {
    char clean_path[1024];
    if (realpath(path, clean_path)) {
        strncpy(current_path, clean_path, sizeof(current_path) - 1);
    } else {
        strncpy(current_path, path, sizeof(current_path) - 1);
    }
    current_path[sizeof(current_path) - 1] = '\0';

    item_count = 0;
    dir_count = 0;
    file_count = 0;

    DIR *d = opendir(current_path);
    if (!d) {
        snprintf(status_text, sizeof(status_text), "Error: Cannot open directory %s", current_path);
        return;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL && item_count < MAX_ITEMS) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (!show_hidden && de->d_name[0] == '.')
            continue;

        FileItem *fi = &items[item_count];
        strncpy(fi->name, de->d_name, sizeof(fi->name) - 1);
        fi->name[sizeof(fi->name) - 1] = '\0';

        char fullpath[2048];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", current_path, de->d_name);

        struct stat lst;
        if (lstat(fullpath, &lst) == 0) {
            fi->is_link = S_ISLNK(lst.st_mode);
            format_mode(lst.st_mode, fi->mode_str);
            fi->size = lst.st_size;
            fi->mtime = lst.st_mtime;
            format_date(lst.st_mtime, fi->date_str, sizeof(fi->date_str));

            struct stat st;
            if (stat(fullpath, &st) == 0) {
                fi->is_dir = S_ISDIR(st.st_mode);
                fi->is_exec = !fi->is_dir && (st.st_mode & 0111);
                if (fi->is_dir) {
                    dir_count++;
                    strcpy(fi->size_str, "-");
                } else {
                    file_count++;
                    format_size(st.st_size, fi->size_str, sizeof(fi->size_str));
                }
            } else {
                fi->is_dir = S_ISDIR(lst.st_mode);
                fi->is_exec = 0;
                format_size(lst.st_size, fi->size_str, sizeof(fi->size_str));
            }
        } else {
            fi->is_dir = 0;
            fi->is_exec = 0;
            fi->is_link = 0;
            strcpy(fi->size_str, "-");
            strcpy(fi->mode_str, "----------");
            strcpy(fi->date_str, "-");
        }
        item_count++;
    }
    closedir(d);

    if (item_count > 1) {
        qsort(items, item_count, sizeof(FileItem), compare_items);
    }

    if (selected_idx >= item_count) selected_idx = item_count - 1;
    if (selected_idx < 0 && item_count > 0) selected_idx = 0;
    scroll_offset = 0;
    update_status();
}

static void go_up_dir(void) {
    if (strcmp(current_path, "/") == 0) return;
    char *slash = strrchr(current_path, '/');
    if (slash) {
        if (slash == current_path) {
            load_directory("/");
        } else {
            *slash = '\0';
            load_directory(current_path);
        }
    }
}

static int rmrf(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return -1;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            char sub[4096];
            snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
            rmrf(sub);
        }
        closedir(d);
        return rmdir(path);
    }
    return unlink(path);
}

static void open_item(int idx) {
    if (idx < 0 || idx >= item_count) return;
    FileItem *fi = &items[idx];

    char fullpath[2048];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", current_path, fi->name);

    if (fi->is_dir) {
        load_directory(fullpath);
        return;
    }

    // Determine handler for file
    const char *ext = strrchr(fi->name, '.');
    char cmd[4096];

    if (fi->is_exec && (!ext || strcmp(ext, ".sh") != 0)) {
        // Binary or executable program
        snprintf(cmd, sizeof(cmd), "\"%s\" &", fullpath);
    } else if (ext && (strcmp(ext, ".zip") == 0)) {
        // Zip archive: extract into current directory
        snprintf(cmd, sizeof(cmd), "sh -c 'unzip -q -o \"%s\" -d \"%s\"' &", fullpath, current_path);
        snprintf(status_text, sizeof(status_text), "Extracting %s ...", fi->name);
    } else if (ext && (strcmp(ext, ".tar") == 0 || strcmp(ext, ".tgz") == 0 ||
                       strcmp(ext, ".gz") == 0 || strcmp(ext, ".bz2") == 0)) {
        // Tar archive: extract into current directory
        snprintf(cmd, sizeof(cmd), "sh -c 'tar -xf \"%s\" -C \"%s\"' &", fullpath, current_path);
        snprintf(status_text, sizeof(status_text), "Extracting %s ...", fi->name);
    } else if (ext && (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0)) {
        // Web document
        snprintf(cmd, sizeof(cmd), "flxbrowser \"%s\" &", fullpath);
    } else if (ext && (strcmp(ext, ".png") == 0 || strcmp(ext, ".ppm") == 0 ||
                       strcmp(ext, ".bmp") == 0 || strcmp(ext, ".jpg") == 0 ||
                       strcmp(ext, ".jpeg") == 0)) {
        // Image viewer
        snprintf(cmd, sizeof(cmd), "flxview \"%s\" &", fullpath);
    } else if (ext && strcmp(ext, ".sh") == 0) {
        // Shell script: run in interactive uxterm
        snprintf(cmd, sizeof(cmd), "uxterm -T 'Script' -e sh -c \"'%s'; echo; printf 'Done. Press Enter...'; read l\" &", fullpath);
    } else {
        // Text / code / config / unknown: open in flxt GUI editor
        snprintf(cmd, sizeof(cmd), "flxt \"%s\" &", fullpath);
    }

    system(cmd);
}

// Dialog openers
static void open_mkdir_dialog(void) {
    dialog_mode = DIALOG_MKDIR;
    strncpy(dialog_title, "Create New Folder", sizeof(dialog_title));
    strncpy(dialog_prompt, "Folder name:", sizeof(dialog_prompt));
    dialog_buf[0] = '\0';
    dialog_cursor = 0;
}

static void open_touch_dialog(void) {
    dialog_mode = DIALOG_TOUCH;
    strncpy(dialog_title, "Create New File", sizeof(dialog_title));
    strncpy(dialog_prompt, "File name:", sizeof(dialog_prompt));
    dialog_buf[0] = '\0';
    dialog_cursor = 0;
}

static void open_rename_dialog(void) {
    if (selected_idx < 0 || selected_idx >= item_count) return;
    dialog_mode = DIALOG_RENAME;
    strncpy(dialog_title, "Rename Item", sizeof(dialog_title));
    snprintf(dialog_prompt, sizeof(dialog_prompt), "Rename '%s' to:", items[selected_idx].name);
    strncpy(dialog_buf, items[selected_idx].name, sizeof(dialog_buf) - 1);
    dialog_buf[sizeof(dialog_buf) - 1] = '\0';
    dialog_cursor = strlen(dialog_buf);
}

static void open_delete_dialog(void) {
    if (selected_idx < 0 || selected_idx >= item_count) return;
    dialog_mode = DIALOG_DELETE;
    strncpy(dialog_title, "Confirm Deletion", sizeof(dialog_title));
    snprintf(dialog_prompt, sizeof(dialog_prompt), "Permanently delete '%s'?", items[selected_idx].name);
    dialog_buf[0] = '\0';
    dialog_cursor = 0;
}

static void open_info_dialog(void) {
    if (selected_idx < 0 || selected_idx >= item_count) return;
    FileItem *fi = &items[selected_idx];
    char full[2048];
    snprintf(full, sizeof(full), "%s/%s", current_path, fi->name);

    struct stat st;
    if (lstat(full, &st) != 0) return;

    dialog_mode = DIALOG_INFO;
    strncpy(dialog_title, "File Properties", sizeof(dialog_title));
    dialog_prompt[0] = '\0';

    dialog_info_line_count = 0;
    snprintf(dialog_info_lines[dialog_info_line_count++], 256, "Name: %s", fi->name);
    snprintf(dialog_info_lines[dialog_info_line_count++], 256, "Type: %s",
             fi->is_dir ? "Directory / Folder" :
             (fi->is_link ? "Symbolic Link" :
             (fi->is_exec ? "Executable Program" : "Regular File")));
    snprintf(dialog_info_lines[dialog_info_line_count++], 256, "Location: %s", current_path);
    snprintf(dialog_info_lines[dialog_info_line_count++], 256, "Size: %s (%ld bytes)", fi->size_str, (long)st.st_size);
    snprintf(dialog_info_lines[dialog_info_line_count++], 256, "Permissions: %s (0%03o)", fi->mode_str, (unsigned int)(st.st_mode & 0777));

    struct passwd *pw = getpwuid(st.st_uid);
    struct group *gr = getgrgid(st.st_gid);
    snprintf(dialog_info_lines[dialog_info_line_count++], 256, "Owner: %s (%u) / %s (%u)",
             pw ? pw->pw_name : "unknown", (unsigned)st.st_uid,
             gr ? gr->gr_name : "unknown", (unsigned)st.st_gid);
    snprintf(dialog_info_lines[dialog_info_line_count++], 256, "Modified: %s", fi->date_str);
}

static void open_goto_dialog(void) {
    dialog_mode = DIALOG_GOTO;
    strncpy(dialog_title, "Go to Folder", sizeof(dialog_title));
    strncpy(dialog_prompt, "Enter directory path:", sizeof(dialog_prompt));
    strncpy(dialog_buf, current_path, sizeof(dialog_buf) - 1);
    dialog_buf[sizeof(dialog_buf) - 1] = '\0';
    dialog_cursor = strlen(dialog_buf);
}

static void commit_dialog_action(void) {
    switch (dialog_mode) {
        case DIALOG_MKDIR:
            if (dialog_buf[0] != '\0') {
                char newpath[2048];
                snprintf(newpath, sizeof(newpath), "%s/%s", current_path, dialog_buf);
                if (mkdir(newpath, 0755) == 0) {
                    load_directory(current_path);
                    for (int i = 0; i < item_count; i++) {
                        if (strcmp(items[i].name, dialog_buf) == 0) {
                            selected_idx = i;
                            break;
                        }
                    }
                    snprintf(status_text, sizeof(status_text), "Created folder: %s", dialog_buf);
                } else {
                    snprintf(status_text, sizeof(status_text), "Error: %s", strerror(errno));
                }
            }
            break;

        case DIALOG_TOUCH:
            if (dialog_buf[0] != '\0') {
                char newpath[2048];
                snprintf(newpath, sizeof(newpath), "%s/%s", current_path, dialog_buf);
                int fd = open(newpath, O_WRONLY | O_CREAT | O_EXCL, 0644);
                if (fd >= 0) {
                    close(fd);
                    load_directory(current_path);
                    for (int i = 0; i < item_count; i++) {
                        if (strcmp(items[i].name, dialog_buf) == 0) {
                            selected_idx = i;
                            break;
                        }
                    }
                    snprintf(status_text, sizeof(status_text), "Created file: %s", dialog_buf);
                } else {
                    snprintf(status_text, sizeof(status_text), "Error: %s", strerror(errno));
                }
            }
            break;

        case DIALOG_RENAME:
            if (selected_idx >= 0 && selected_idx < item_count && dialog_buf[0] != '\0') {
                char oldpath[2048], newpath[2048];
                snprintf(oldpath, sizeof(oldpath), "%s/%s", current_path, items[selected_idx].name);
                snprintf(newpath, sizeof(newpath), "%s/%s", current_path, dialog_buf);
                if (rename(oldpath, newpath) == 0) {
                    load_directory(current_path);
                    for (int i = 0; i < item_count; i++) {
                        if (strcmp(items[i].name, dialog_buf) == 0) {
                            selected_idx = i;
                            break;
                        }
                    }
                    snprintf(status_text, sizeof(status_text), "Renamed to: %s", dialog_buf);
                } else {
                    snprintf(status_text, sizeof(status_text), "Error renaming: %s", strerror(errno));
                }
            }
            break;

        case DIALOG_DELETE:
            if (selected_idx >= 0 && selected_idx < item_count) {
                char delpath[2048];
                char name[256];
                strncpy(name, items[selected_idx].name, sizeof(name) - 1);
                name[sizeof(name) - 1] = '\0';
                snprintf(delpath, sizeof(delpath), "%s/%s", current_path, name);
                if (rmrf(delpath) == 0) {
                    load_directory(current_path);
                    snprintf(status_text, sizeof(status_text), "Deleted: %s", name);
                } else {
                    snprintf(status_text, sizeof(status_text), "Error deleting: %s", strerror(errno));
                }
            }
            break;

        case DIALOG_GOTO:
            if (dialog_buf[0] != '\0') {
                load_directory(dialog_buf);
            }
            break;

        default:
            break;
    }
    dialog_mode = DIALOG_NONE;
}

// Clearlooks / Standard Clean Desktop Color Palette
static void draw_border_rect(cairo_t *cr, double x, double y, double w, double h, double r, double g, double b) {
    cairo_set_source_rgb(cr, r, g, b);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0);
    cairo_stroke(cr);
}

static void draw_dialog(cairo_t *cr, int win_w, int win_h) {
    if (dialog_mode == DIALOG_NONE) return;

    int dw = (dialog_mode == DIALOG_INFO) ? 500 : 440;
    int dh = (dialog_mode == DIALOG_INFO) ? 250 : 160;
    int dx = (win_w - dw) / 2;
    int dy = (win_h - dh) / 2;

    // Dim background overlay
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.35);
    cairo_rectangle(cr, 0, 0, win_w, win_h);
    cairo_fill(cr);

    // Dialog drop shadow
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.20);
    cairo_rectangle(cr, dx + 4, dy + 4, dw, dh);
    cairo_fill(cr);

    // Dialog background
    cairo_set_source_rgb(cr, 0.96, 0.96, 0.96);
    cairo_rectangle(cr, dx, dy, dw, dh);
    cairo_fill(cr);

    // Dialog border
    cairo_set_source_rgb(cr, 0.30, 0.55, 0.85);
    cairo_set_line_width(cr, 1.5);
    cairo_rectangle(cr, dx + 0.5, dy + 0.5, dw - 1.0, dh - 1.0);
    cairo_stroke(cr);

    // Title bar
    cairo_set_source_rgb(cr, 0.29, 0.56, 0.85);
    cairo_rectangle(cr, dx, dy, dw, 28);
    cairo_fill(cr);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12.0);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_move_to(cr, dx + 12, dy + 19);
    cairo_show_text(cr, dialog_title);

    // Content area
    if (dialog_mode == DIALOG_MKDIR || dialog_mode == DIALOG_TOUCH ||
        dialog_mode == DIALOG_RENAME || dialog_mode == DIALOG_GOTO) {
        // Prompt text
        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.5);
        cairo_move_to(cr, dx + 18, dy + 55);
        cairo_show_text(cr, dialog_prompt);

        // Input text box
        int ib_x = dx + 18;
        int ib_y = dy + 68;
        int ib_w = dw - 36;
        int ib_h = 26;
        draw_border_rect(cr, ib_x, ib_y, ib_w, ib_h, 1.0, 1.0, 1.0);

        // Input text
        cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.5);
        cairo_move_to(cr, ib_x + 8, ib_y + 17);
        cairo_show_text(cr, dialog_buf);

        // Cursor
        cairo_text_extents_t cur_ext;
        cairo_text_extents(cr, dialog_buf, &cur_ext);
        cairo_set_source_rgb(cr, 0.29, 0.56, 0.85);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, ib_x + 8 + cur_ext.x_advance + 1, ib_y + 4);
        cairo_line_to(cr, ib_x + 8 + cur_ext.x_advance + 1, ib_y + ib_h - 4);
        cairo_stroke(cr);

        // Buttons: [ OK ] and [ Cancel ]
        dlg_btn1_w = 72; dlg_btn1_h = 24;
        dlg_btn1_x = dx + dw - 168; dlg_btn1_y = dy + dh - 36;

        dlg_btn2_w = 72; dlg_btn2_h = 24;
        dlg_btn2_x = dx + dw - 86;  dlg_btn2_y = dy + dh - 36;

        // OK button (accent blue)
        cairo_set_source_rgb(cr, 0.29, 0.56, 0.85);
        cairo_rectangle(cr, dlg_btn1_x, dlg_btn1_y, dlg_btn1_w, dlg_btn1_h);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11.0);
        cairo_text_extents_t ok_ext;
        cairo_text_extents(cr, "OK", &ok_ext);
        cairo_move_to(cr, dlg_btn1_x + (dlg_btn1_w - ok_ext.width) / 2.0 - ok_ext.x_bearing,
                          dlg_btn1_y + (dlg_btn1_h - ok_ext.height) / 2.0 - ok_ext.y_bearing);
        cairo_show_text(cr, "OK");

        // Cancel button (grey)
        draw_border_rect(cr, dlg_btn2_x, dlg_btn2_y, dlg_btn2_w, dlg_btn2_h, 0.94, 0.94, 0.94);
        cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
        cairo_text_extents_t can_ext;
        cairo_text_extents(cr, "Cancel", &can_ext);
        cairo_move_to(cr, dlg_btn2_x + (dlg_btn2_w - can_ext.width) / 2.0 - can_ext.x_bearing,
                          dlg_btn2_y + (dlg_btn2_h - can_ext.height) / 2.0 - can_ext.y_bearing);
        cairo_show_text(cr, "Cancel");

    } else if (dialog_mode == DIALOG_DELETE) {
        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12.0);
        cairo_move_to(cr, dx + 20, dy + 60);
        cairo_show_text(cr, dialog_prompt);

        cairo_set_source_rgb(cr, 0.7, 0.2, 0.2);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.0);
        cairo_move_to(cr, dx + 20, dy + 82);
        cairo_show_text(cr, "Warning: This action cannot be undone!");

        dlg_btn1_w = 80; dlg_btn1_h = 24;
        dlg_btn1_x = dx + dw - 176; dlg_btn1_y = dy + dh - 36;

        dlg_btn2_w = 76; dlg_btn2_h = 24;
        dlg_btn2_x = dx + dw - 86;  dlg_btn2_y = dy + dh - 36;

        // Delete button (Red accent)
        cairo_set_source_rgb(cr, 0.82, 0.22, 0.22);
        cairo_rectangle(cr, dlg_btn1_x, dlg_btn1_y, dlg_btn1_w, dlg_btn1_h);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11.0);
        cairo_text_extents_t del_ext;
        cairo_text_extents(cr, "Delete", &del_ext);
        cairo_move_to(cr, dlg_btn1_x + (dlg_btn1_w - del_ext.width) / 2.0 - del_ext.x_bearing,
                          dlg_btn1_y + (dlg_btn1_h - del_ext.height) / 2.0 - del_ext.y_bearing);
        cairo_show_text(cr, "Delete");

        // Cancel button
        draw_border_rect(cr, dlg_btn2_x, dlg_btn2_y, dlg_btn2_w, dlg_btn2_h, 0.94, 0.94, 0.94);
        cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
        cairo_text_extents_t can_ext;
        cairo_text_extents(cr, "Cancel", &can_ext);
        cairo_move_to(cr, dlg_btn2_x + (dlg_btn2_w - can_ext.width) / 2.0 - can_ext.x_bearing,
                          dlg_btn2_y + (dlg_btn2_h - can_ext.height) / 2.0 - can_ext.y_bearing);
        cairo_show_text(cr, "Cancel");

    } else if (dialog_mode == DIALOG_INFO) {
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.0);
        for (int i = 0; i < dialog_info_line_count; i++) {
            cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
            cairo_move_to(cr, dx + 20, dy + 52 + i * 22);
            cairo_show_text(cr, dialog_info_lines[i]);
        }

        dlg_btn1_w = 80; dlg_btn1_h = 24;
        dlg_btn1_x = dx + (dw - dlg_btn1_w) / 2; dlg_btn1_y = dy + dh - 34;

        // Close button (accent blue)
        cairo_set_source_rgb(cr, 0.29, 0.56, 0.85);
        cairo_rectangle(cr, dlg_btn1_x, dlg_btn1_y, dlg_btn1_w, dlg_btn1_h);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11.0);
        cairo_text_extents_t cls_ext;
        cairo_text_extents(cr, "Close", &cls_ext);
        cairo_move_to(cr, dlg_btn1_x + (dlg_btn1_w - cls_ext.width) / 2.0 - cls_ext.x_bearing,
                          dlg_btn1_y + (dlg_btn1_h - cls_ext.height) / 2.0 - cls_ext.y_bearing);
        cairo_show_text(cr, "Close");
    }
}

static void draw_ui(cairo_t *cr, int win_w, int win_h) {
    layout_buttons();

    // 1. Overall window background: pure white
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    // 2. Toolbar: Clearlooks light grey #ebebeb
    cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
    cairo_rectangle(cr, 0, 0, win_w, TOP_BAR_H);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.70, 0.70, 0.70);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, TOP_BAR_H + 0.5);
    cairo_line_to(cr, win_w, TOP_BAR_H + 0.5);
    cairo_stroke(cr);

    // Buttons on Toolbar
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.5);

    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        Button *b = &buttons[i];
        int active = (b->id == BTN_HIDDEN && show_hidden);
        if (active) {
            cairo_set_source_rgb(cr, 0.29, 0.56, 0.85);
            cairo_rectangle(cr, b->x, b->y, b->w, b->h);
            cairo_fill(cr);
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        } else {
            draw_border_rect(cr, b->x, b->y, b->w, b->h, 0.96, 0.96, 0.96);
            cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        }

        cairo_text_extents_t ext;
        cairo_text_extents(cr, b->label, &ext);
        cairo_move_to(cr, b->x + (b->w - ext.width) / 2.0 - ext.x_bearing,
                          b->y + (b->h - ext.height) / 2.0 - ext.y_bearing);
        cairo_show_text(cr, b->label);
    }

    // Right logo in Toolbar: "FreeLinX Files"
    cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11.0);
    cairo_text_extents_t logo_ext;
    cairo_text_extents(cr, "FreeLinX Files", &logo_ext);
    if (win_w - logo_ext.width - 12 > 620) {
        cairo_move_to(cr, win_w - logo_ext.width - 12, 21);
        cairo_show_text(cr, "FreeLinX Files");
    }

    // 3. Location / Path Bar
    int path_y = TOP_BAR_H + 1;
    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_rectangle(cr, 0, path_y, win_w, PATH_BAR_H);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.80, 0.80, 0.80);
    cairo_move_to(cr, 0, path_y + PATH_BAR_H + 0.5);
    cairo_line_to(cr, win_w, path_y + PATH_BAR_H + 0.5);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11.0);
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_move_to(cr, 10, path_y + 17);
    cairo_show_text(cr, "Location:");

    // Current path text inside a clean clickable input box
    double pbox_x = 76;
    double pbox_w = win_w - pbox_x - 10;
    draw_border_rect(cr, pbox_x, path_y + 3, pbox_w, PATH_BAR_H - 6, 1.0, 1.0, 1.0);
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11.0);
    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    cairo_move_to(cr, pbox_x + 6, path_y + 16);
    cairo_show_text(cr, current_path);

    // 4. Table Header
    int hdr_y = path_y + PATH_BAR_H + 1;
    cairo_set_source_rgb(cr, 0.88, 0.88, 0.88);
    cairo_rectangle(cr, 0, hdr_y, win_w, HEADER_H);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
    cairo_move_to(cr, 0, hdr_y + HEADER_H + 0.5);
    cairo_line_to(cr, win_w, hdr_y + HEADER_H + 0.5);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11.0);
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);

    // Column positions
    int col_size = win_w - 370 - SCROLL_W;
    int col_mode = win_w - 290 - SCROLL_W;
    int col_date = win_w - 170 - SCROLL_W;

    cairo_move_to(cr, 12, hdr_y + 15);
    cairo_show_text(cr, "Name");

    cairo_move_to(cr, col_size, hdr_y + 15);
    cairo_show_text(cr, "Size");

    cairo_move_to(cr, col_mode, hdr_y + 15);
    cairo_show_text(cr, "Permissions");

    cairo_move_to(cr, col_date, hdr_y + 15);
    cairo_show_text(cr, "Date Modified");

    // Vertical column dividers in header
    cairo_move_to(cr, col_size - 10 + 0.5, hdr_y);
    cairo_line_to(cr, col_size - 10 + 0.5, hdr_y + HEADER_H);
    cairo_move_to(cr, col_mode - 10 + 0.5, hdr_y);
    cairo_line_to(cr, col_mode - 10 + 0.5, hdr_y + HEADER_H);
    cairo_move_to(cr, col_date - 10 + 0.5, hdr_y);
    cairo_line_to(cr, col_date - 10 + 0.5, hdr_y + HEADER_H);
    cairo_stroke(cr);

    // 5. File List Area
    int list_y = hdr_y + HEADER_H + 1;
    int list_h = win_h - list_y - STATUS_BAR_H - 1;
    int list_w = win_w - SCROLL_W;
    int visible_rows = list_h / ROW_H;

    for (int r = 0; r < visible_rows; r++) {
        int idx = scroll_offset + r;
        if (idx >= item_count) break;

        FileItem *fi = &items[idx];
        int ry = list_y + r * ROW_H;
        int is_sel = (idx == selected_idx);

        if (is_sel) {
            // Clearlooks Selection Blue
            cairo_set_source_rgb(cr, 0.29, 0.56, 0.85);
            cairo_rectangle(cr, 0, ry, list_w, ROW_H);
            cairo_fill(cr);
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        } else {
            // Alternating clean white / subtle grey
            if (r % 2 == 1) {
                cairo_set_source_rgb(cr, 0.97, 0.97, 0.97);
                cairo_rectangle(cr, 0, ry, list_w, ROW_H);
                cairo_fill(cr);
            }
            cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        }

        // Draw item badge
        cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 10.0);
        if (!is_sel) {
            if (fi->is_dir) cairo_set_source_rgb(cr, 0.65, 0.35, 0.0);      // Amber for folder
            else if (fi->is_exec) cairo_set_source_rgb(cr, 0.0, 0.50, 0.0); // Green for executable
            else if (fi->is_link) cairo_set_source_rgb(cr, 0.0, 0.25, 0.7); // Blue for symlink
            else cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
        }
        const char *badge = fi->is_dir ? "[DIR]" : (fi->is_exec ? "[EXE]" : (fi->is_link ? "[LNK]" : "     "));
        cairo_move_to(cr, 10, ry + 15);
        cairo_show_text(cr, badge);

        // Filename
        if (!is_sel) {
            cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
            if (fi->is_dir) {
                cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            } else {
                cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            }
        } else {
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        }
        cairo_set_font_size(cr, 11.0);
        cairo_move_to(cr, 55, ry + 15);
        cairo_show_text(cr, fi->name);

        // Size
        if (!is_sel) cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
        cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 10.5);
        cairo_move_to(cr, col_size, ry + 15);
        cairo_show_text(cr, fi->size_str);

        // Permissions
        cairo_move_to(cr, col_mode, ry + 15);
        cairo_show_text(cr, fi->mode_str);

        // Date
        cairo_move_to(cr, col_date, ry + 15);
        cairo_show_text(cr, fi->date_str);
    }

    // 6. Scrollbar
    int sb_x = win_w - SCROLL_W;
    cairo_set_source_rgb(cr, 0.918, 0.933, 0.933);
    cairo_rectangle(cr, sb_x, list_y, SCROLL_W, list_h);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.80, 0.80, 0.80);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, sb_x + 0.5, list_y);
    cairo_line_to(cr, sb_x + 0.5, list_y + list_h);
    cairo_stroke(cr);

    if (item_count > visible_rows && item_count > 0) {
        double thumb_ratio = (double)visible_rows / (double)item_count;
        double thumb_h = list_h * thumb_ratio;
        if (thumb_h < 20.0) thumb_h = 20.0;
        double max_scroll = item_count - visible_rows;
        double thumb_y = list_y + (list_h - thumb_h) * ((double)scroll_offset / max_scroll);
        draw_border_rect(cr, sb_x + 1, thumb_y, SCROLL_W - 2, thumb_h, 0.75, 0.75, 0.75);
    }

    // 7. Status Bar at bottom
    int stat_y = win_h - STATUS_BAR_H;
    cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
    cairo_rectangle(cr, 0, stat_y, win_w, STATUS_BAR_H);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.80, 0.80, 0.80);
    cairo_move_to(cr, 0, stat_y + 0.5);
    cairo_line_to(cr, win_w, stat_y + 0.5);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.5);
    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    cairo_move_to(cr, 10, stat_y + 15);
    cairo_show_text(cr, status_text);

    // 8. If modal dialog active, draw on top
    if (dialog_mode != DIALOG_NONE) {
        draw_dialog(cr, win_w, win_h);
    }
}

int main(int argc, char **argv) {
    const char *start_path = "/root";
    if (argc > 1 && argv[1][0] != '\0') {
        start_path = argv[1];
    } else {
        const char *home = getenv("HOME");
        if (home && home[0] != '\0') start_path = home;
    }
    load_directory(start_path);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "flxfm: Cannot open X display\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    XSetWindowAttributes swa;
    swa.background_pixel = WhitePixel(dpy, screen);
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | StructureNotifyMask;

    int win_w = WIN_DEFAULT_W;
    int win_h = WIN_DEFAULT_H;

    Window win = XCreateWindow(dpy, root, 60, 60, win_w, win_h, 0,
                               DefaultDepth(dpy, screen), InputOutput,
                               DefaultVisual(dpy, screen),
                               CWBackPixel | CWEventMask, &swa);

    char win_title[1024];
    snprintf(win_title, sizeof(win_title), "FreeLinX Files - %s", current_path);
    XStoreName(dpy, win, win_title);
    XClassHint ch = {"flxfm", "FreeLinX"};
    XSetClassHint(dpy, win, &ch);

    XSizeHints hints;
    hints.flags = USPosition | USSize | PMinSize;
    hints.x = 60;
    hints.y = 60;
    hints.width = win_w;
    hints.height = win_h;
    hints.min_width = 480;
    hints.min_height = 300;
    XSetWMNormalHints(dpy, win, &hints);

    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    XMapWindow(dpy, win);
    XFlush(dpy);

    // Double-buffering: create pixmap
    Pixmap backbuffer = XCreatePixmap(dpy, win, win_w, win_h, DefaultDepth(dpy, screen));
    GC gc = XCreateGC(dpy, win, 0, NULL);

    cairo_surface_t *cs = cairo_xlib_surface_create(dpy, backbuffer, DefaultVisual(dpy, screen), win_w, win_h);
    cairo_t *cr = cairo_create(cs);

    int dragging_scrollbar = 0;
    int running = 1;

    // Initial render
    draw_ui(cr, win_w, win_h);
    XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
    XFlush(dpy);

    while (running) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) {
                    XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
                    XFlush(dpy);
                }
                break;

            case ConfigureNotify:
                if (ev.xconfigure.width != win_w || ev.xconfigure.height != win_h) {
                    win_w = ev.xconfigure.width;
                    win_h = ev.xconfigure.height;

                    cairo_destroy(cr);
                    cairo_surface_destroy(cs);
                    XFreePixmap(dpy, backbuffer);

                    backbuffer = XCreatePixmap(dpy, win, win_w, win_h, DefaultDepth(dpy, screen));
                    cs = cairo_xlib_surface_create(dpy, backbuffer, DefaultVisual(dpy, screen), win_w, win_h);
                    cr = cairo_create(cs);

                    draw_ui(cr, win_w, win_h);
                    XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
                    XFlush(dpy);
                }
                break;

            case ButtonPress: {
                int mx = ev.xbutton.x;
                int my = ev.xbutton.y;
                int btn = ev.xbutton.button;

                // Handle Dialog interactions first if open
                if (dialog_mode != DIALOG_NONE) {
                    if (btn == 1) {
                        // Check Button 1 (OK / Delete / Close)
                        if (mx >= dlg_btn1_x && mx < dlg_btn1_x + dlg_btn1_w &&
                            my >= dlg_btn1_y && my < dlg_btn1_y + dlg_btn1_h) {
                            commit_dialog_action();
                            snprintf(win_title, sizeof(win_title), "FreeLinX Files - %s", current_path);
                            XStoreName(dpy, win, win_title);
                            draw_ui(cr, win_w, win_h);
                            XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
                            XFlush(dpy);
                            break;
                        }
                        // Check Button 2 (Cancel)
                        if (dialog_mode != DIALOG_INFO &&
                            mx >= dlg_btn2_x && mx < dlg_btn2_x + dlg_btn2_w &&
                            my >= dlg_btn2_y && my < dlg_btn2_y + dlg_btn2_h) {
                            dialog_mode = DIALOG_NONE;
                            draw_ui(cr, win_w, win_h);
                            XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
                            XFlush(dpy);
                            break;
                        }
                    }
                    // Consume all clicks while dialog is open
                    break;
                }

                // Mouse wheel up (Button 4)
                if (btn == 4) {
                    if (scroll_offset > 0) {
                        scroll_offset -= 3;
                        if (scroll_offset < 0) scroll_offset = 0;
                        draw_ui(cr, win_w, win_h);
                        XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
                        XFlush(dpy);
                    }
                    break;
                }
                // Mouse wheel down (Button 5)
                if (btn == 5) {
                    int list_y = TOP_BAR_H + 1 + PATH_BAR_H + 1 + HEADER_H + 1;
                    int list_h = win_h - list_y - STATUS_BAR_H - 1;
                    int visible_rows = list_h / ROW_H;
                    if (scroll_offset + visible_rows < item_count) {
                        scroll_offset += 3;
                        if (scroll_offset > item_count - visible_rows)
                            scroll_offset = item_count - visible_rows;
                        draw_ui(cr, win_w, win_h);
                        XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
                        XFlush(dpy);
                    }
                    break;
                }

                if (btn == 1 || btn == 3) {
                    // Check Toolbar Buttons
                    if (my < TOP_BAR_H) {
                        for (size_t i = 0; i < NUM_BUTTONS; i++) {
                            Button *b = &buttons[i];
                            if (mx >= b->x && mx < b->x + b->w && my >= b->y && my < b->y + b->h) {
                                switch (b->id) {
                                    case BTN_ROOT:
                                        load_directory("/");
                                        break;
                                    case BTN_HOME: {
                                        const char *h = getenv("HOME");
                                        load_directory(h ? h : "/root");
                                        break;
                                    }
                                    case BTN_UP:
                                        go_up_dir();
                                        break;
                                    case BTN_REFRESH:
                                        load_directory(current_path);
                                        break;
                                    case BTN_MKDIR:
                                        open_mkdir_dialog();
                                        break;
                                    case BTN_TOUCH:
                                        open_touch_dialog();
                                        break;
                                    case BTN_RENAME:
                                        open_rename_dialog();
                                        break;
                                    case BTN_DELETE:
                                        open_delete_dialog();
                                        break;
                                    case BTN_INFO:
                                        open_info_dialog();
                                        break;
                                    case BTN_HIDDEN:
                                        show_hidden = !show_hidden;
                                        load_directory(current_path);
                                        break;
                                    case BTN_TERM: {
                                        char tcmd[2048];
                                        snprintf(tcmd, sizeof(tcmd), "cd \"%s\" && uxterm &", current_path);
                                        system(tcmd);
                                        break;
                                    }
                                }
                                snprintf(win_title, sizeof(win_title), "FreeLinX Files - %s", current_path);
                                XStoreName(dpy, win, win_title);
                                draw_ui(cr, win_w, win_h);
                                XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
                                XFlush(dpy);
                                break;
                            }
                        }
                        break;
                    }

                    // Check Path Bar Click -> Go to dialog
                    int path_y = TOP_BAR_H + 1;
                    if (my >= path_y && my < path_y + PATH_BAR_H) {
                        open_goto_dialog();
                        draw_ui(cr, win_w, win_h);
                        XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
                        XFlush(dpy);
                        break;
                    }

                    // Check File List click
                    int list_y = TOP_BAR_H + 1 + PATH_BAR_H + 1 + HEADER_H + 1;
                    int list_h = win_h - list_y - STATUS_BAR_H - 1;
                    int list_w = win_w - SCROLL_W;

                    if (mx < list_w && my >= list_y && my < list_y + list_h) {
                        int row = (my - list_y) / ROW_H;
                        int clicked_idx = scroll_offset + row;
                        if (clicked_idx >= 0 && clicked_idx < item_count) {
                            selected_idx = clicked_idx;
                            update_status();

                            struct timespec now;
                            clock_gettime(CLOCK_MONOTONIC, &now);
                            long diff_ms = (now.tv_sec - last_click_time.tv_sec) * 1000 +
                                           (now.tv_nsec - last_click_time.tv_nsec) / 1000000;

                            if (btn == 3) {
                                // Right click opens item immediately!
                                open_item(selected_idx);
                                snprintf(win_title, sizeof(win_title), "FreeLinX Files - %s", current_path);
                                XStoreName(dpy, win, win_title);
                            } else if (clicked_idx == last_clicked_idx && diff_ms < 450) {
                                // Double click
                                open_item(selected_idx);
                                snprintf(win_title, sizeof(win_title), "FreeLinX Files - %s", current_path);
                                XStoreName(dpy, win, win_title);
                                last_clicked_idx = -1;
                            } else {
                                last_clicked_idx = clicked_idx;
                                last_click_time = now;
                            }

                            draw_ui(cr, win_w, win_h);
                            XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
                            XFlush(dpy);
                        }
                    }

                    // Check Scrollbar click
                    if (mx >= list_w && my >= list_y && my < list_y + list_h) {
                        dragging_scrollbar = 1;
                        int visible_rows = list_h / ROW_H;
                        double ratio = (double)(my - list_y) / (double)list_h;
                        scroll_offset = (int)(ratio * item_count);
                        if (scroll_offset > item_count - visible_rows)
                            scroll_offset = item_count - visible_rows;
                        if (scroll_offset < 0) scroll_offset = 0;
                        draw_ui(cr, win_w, win_h);
                        XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
                        XFlush(dpy);
                    }
                }
                break;
            }

            case ButtonRelease:
                dragging_scrollbar = 0;
                break;

            case MotionNotify:
                if (dragging_scrollbar) {
                    int my = ev.xmotion.y;
                    int list_y = TOP_BAR_H + 1 + PATH_BAR_H + 1 + HEADER_H + 1;
                    int list_h = win_h - list_y - STATUS_BAR_H - 1;
                    int visible_rows = list_h / ROW_H;
                    double ratio = (double)(my - list_y) / (double)list_h;
                    scroll_offset = (int)(ratio * item_count);
                    if (scroll_offset > item_count - visible_rows)
                        scroll_offset = item_count - visible_rows;
                    if (scroll_offset < 0) scroll_offset = 0;
                    draw_ui(cr, win_w, win_h);
                    XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
                    XFlush(dpy);
                }
                break;

            case KeyPress: {
                char text_buf[32];
                KeySym ks;
                int len = XLookupString(&ev.xkey, text_buf, sizeof(text_buf) - 1, &ks, NULL);
                if (len > 0) text_buf[len] = '\0';

                // Handle Dialog key events
                if (dialog_mode != DIALOG_NONE) {
                    if (ks == XK_Escape) {
                        dialog_mode = DIALOG_NONE;
                    } else if (ks == XK_Return) {
                        commit_dialog_action();
                        snprintf(win_title, sizeof(win_title), "FreeLinX Files - %s", current_path);
                        XStoreName(dpy, win, win_title);
                    } else if (dialog_mode == DIALOG_DELETE) {
                        if (ks == XK_y || ks == XK_Y) {
                            commit_dialog_action();
                            snprintf(win_title, sizeof(win_title), "FreeLinX Files - %s", current_path);
                            XStoreName(dpy, win, win_title);
                        } else if (ks == XK_n || ks == XK_N) {
                            dialog_mode = DIALOG_NONE;
                        }
                    } else if (dialog_mode == DIALOG_INFO) {
                        if (ks == XK_space) {
                            dialog_mode = DIALOG_NONE;
                        }
                    } else {
                        // Text input dialogs (MKDIR, TOUCH, RENAME, GOTO)
                        if (ks == XK_BackSpace) {
                            if (dialog_cursor > 0) {
                                dialog_buf[--dialog_cursor] = '\0';
                            }
                        } else if (len > 0 && (unsigned char)text_buf[0] >= 32 && (unsigned char)text_buf[0] != 127) {
                            if (dialog_cursor < (int)sizeof(dialog_buf) - 2) {
                                dialog_buf[dialog_cursor++] = text_buf[0];
                                dialog_buf[dialog_cursor] = '\0';
                            }
                        }
                    }

                    draw_ui(cr, win_w, win_h);
                    XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
                    XFlush(dpy);
                    break;
                }

                // Standard File Manager Keybindings
                int list_y = TOP_BAR_H + 1 + PATH_BAR_H + 1 + HEADER_H + 1;
                int list_h = win_h - list_y - STATUS_BAR_H - 1;
                int visible_rows = list_h / ROW_H;

                if (ks == XK_Return) {
                    open_item(selected_idx);
                    snprintf(win_title, sizeof(win_title), "FreeLinX Files - %s", current_path);
                    XStoreName(dpy, win, win_title);
                } else if (ks == XK_Down) {
                    if (selected_idx < item_count - 1) {
                        selected_idx++;
                        if (selected_idx >= scroll_offset + visible_rows) {
                            scroll_offset = selected_idx - visible_rows + 1;
                        }
                        update_status();
                    }
                } else if (ks == XK_Up) {
                    if (selected_idx > 0) {
                        selected_idx--;
                        if (selected_idx < scroll_offset) {
                            scroll_offset = selected_idx;
                        }
                        update_status();
                    }
                } else if (ks == XK_Page_Down) {
                    selected_idx += visible_rows;
                    if (selected_idx >= item_count) selected_idx = item_count - 1;
                    scroll_offset += visible_rows;
                    if (scroll_offset > item_count - visible_rows)
                        scroll_offset = item_count - visible_rows;
                    if (scroll_offset < 0) scroll_offset = 0;
                    update_status();
                } else if (ks == XK_Page_Up) {
                    selected_idx -= visible_rows;
                    if (selected_idx < 0) selected_idx = 0;
                    scroll_offset -= visible_rows;
                    if (scroll_offset < 0) scroll_offset = 0;
                    update_status();
                } else if (ks == XK_Home) {
                    selected_idx = 0;
                    scroll_offset = 0;
                    update_status();
                } else if (ks == XK_End) {
                    selected_idx = item_count - 1;
                    scroll_offset = item_count - visible_rows;
                    if (scroll_offset < 0) scroll_offset = 0;
                    update_status();
                } else if (ks == XK_BackSpace) {
                    go_up_dir();
                    snprintf(win_title, sizeof(win_title), "FreeLinX Files - %s", current_path);
                    XStoreName(dpy, win, win_title);
                } else if (ks == XK_F5 || ks == XK_r) {
                    load_directory(current_path);
                } else if (ks == XK_h) {
                    show_hidden = !show_hidden;
                    load_directory(current_path);
                } else if (ks == XK_n) {
                    open_mkdir_dialog();
                } else if (ks == XK_c) {
                    open_touch_dialog();
                } else if (ks == XK_F2) {
                    open_rename_dialog();
                } else if (ks == XK_Delete || ks == XK_d) {
                    open_delete_dialog();
                } else if (ks == XK_i) {
                    open_info_dialog();
                } else if (ks == XK_slash || ((ev.xkey.state & ControlMask) && (ks == XK_l || ks == XK_L))) {
                    open_goto_dialog();
                } else if (ks == XK_Escape || ks == XK_q) {
                    running = 0;
                }

                draw_ui(cr, win_w, win_h);
                XCopyArea(dpy, backbuffer, win, gc, 0, 0, win_w, win_h, 0, 0);
                XFlush(dpy);
                break;
            }

            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == wm_delete) {
                    running = 0;
                }
                break;
        }
    }

    cairo_destroy(cr);
    cairo_surface_destroy(cs);
    XFreePixmap(dpy, backbuffer);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
