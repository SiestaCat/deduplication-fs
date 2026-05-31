/*
 * deduplication-fs GUI — GTK3.
 *
 * Lets the user pick a backing-storage directory and a mount point, then
 * spawns the FUSE binary ("app", looked up next to this executable). On
 * mount it adds a bookmark for the mount point so it shows up in the file
 * manager's sidebar (Nautilus, Nemo, Files, etc.), and opens the folder.
 */

#define _POSIX_C_SOURCE 200809L

#include <gtk/gtk.h>

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BOOKMARK_LABEL "deduplication-fs"
#define APP_ID         "dev.dedup.fs.gui"

typedef struct {
    GtkWidget *window;
    GtkWidget *storage_chooser;
    GtkWidget *mount_chooser;
    GtkWidget *toggle_btn;
    GtkWidget *status_label;
    GtkWidget *log_view;
    GtkTextBuffer *log_buf;

    gchar *storage_path;
    gchar *mount_path;
    GPid   fuse_pid;
    gboolean mounted;
} AppState;

/* ---------- utility ---------- */

static void status_set(AppState *st, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void status_set(AppState *st, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(st->status_label), msg);
    g_free(msg);
}

static void log_append(AppState *st, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void log_append(AppState *st, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(st->log_buf, &end);
    gtk_text_buffer_insert(st->log_buf, &end, msg, -1);
    gtk_text_buffer_insert(st->log_buf, &end, "\n", 1);
    GtkTextMark *mark = gtk_text_buffer_get_insert(st->log_buf);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(st->log_view), mark, 0.0, TRUE, 0.0, 1.0);
    g_free(msg);
}

/* ---------- config persistence ---------- */

static gchar *config_path(void) {
    return g_build_filename(g_get_user_config_dir(), "dedup-fs", "config", NULL);
}

static void config_save(const char *storage, const char *mount) {
    GKeyFile *kf = g_key_file_new();
    if (storage) g_key_file_set_string(kf, "paths", "storage", storage);
    if (mount)   g_key_file_set_string(kf, "paths", "mount",   mount);
    gchar *path = config_path();
    gchar *dir  = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    g_key_file_save_to_file(kf, path, NULL);
    g_free(path);
    g_key_file_free(kf);
}

static void config_load(gchar **storage, gchar **mount) {
    *storage = NULL;
    *mount   = NULL;
    GKeyFile *kf = g_key_file_new();
    gchar *path = config_path();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        *storage = g_key_file_get_string(kf, "paths", "storage", NULL);
        *mount   = g_key_file_get_string(kf, "paths", "mount",   NULL);
    }
    g_free(path);
    g_key_file_free(kf);
}

/* ---------- bookmarks ---------- */

static void bookmarks_modify(const char *mount_path, gboolean add) {
    const char *files[] = {
        ".config/gtk-3.0/bookmarks",
        ".config/gtk-4.0/bookmarks",
        NULL
    };
    gchar *uri = g_filename_to_uri(mount_path, NULL, NULL);
    if (!uri) return;
    const char *home = g_get_home_dir();

    for (int i = 0; files[i]; i++) {
        gchar *path = g_build_filename(home, files[i], NULL);
        gchar *dir  = g_path_get_dirname(path);
        g_mkdir_with_parents(dir, 0700);
        g_free(dir);

        gchar *content = NULL;
        gsize len = 0;
        g_file_get_contents(path, &content, &len, NULL);

        GString *out = g_string_new(NULL);
        if (content) {
            char **lines = g_strsplit(content, "\n", -1);
            for (int j = 0; lines[j]; j++) {
                if (*lines[j] == '\0') continue;
                /* drop any prior entry for the same uri */
                if (g_str_has_prefix(lines[j], uri) &&
                    (lines[j][strlen(uri)] == '\0' ||
                     lines[j][strlen(uri)] == ' ')) {
                    continue;
                }
                g_string_append(out, lines[j]);
                g_string_append_c(out, '\n');
            }
            g_strfreev(lines);
            g_free(content);
        }
        if (add) {
            g_string_append_printf(out, "%s %s\n", uri, BOOKMARK_LABEL);
        }
        g_file_set_contents(path, out->str, out->len, NULL);
        g_string_free(out, TRUE);
        g_free(path);
    }
    g_free(uri);
}

/* ---------- FUSE process ---------- */

static char *find_app_binary(void) {
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
        char *dir  = g_path_get_dirname(self);
        char *cand = g_build_filename(dir, "app", NULL);
        g_free(dir);
        if (g_file_test(cand, G_FILE_TEST_IS_EXECUTABLE)) return cand;
        g_free(cand);
    }
    char *path = g_find_program_in_path("dedup-fs");
    if (path) return path;
    return NULL;
}

static void on_child_exit(GPid pid, gint status, gpointer user_data) {
    AppState *st = user_data;
    g_spawn_close_pid(pid);
    if (st->fuse_pid != pid) return;
    st->fuse_pid = 0;
    if (st->mounted) {
        st->mounted = FALSE;
        if (st->mount_path) bookmarks_modify(st->mount_path, FALSE);
        gtk_button_set_label(GTK_BUTTON(st->toggle_btn), "Mount");
        status_set(st, "FUSE process exited unexpectedly (status=%d)", status);
        log_append(st, "[gui] FUSE process exited (status=%d)", status);
    }
}

static gboolean start_fuse(AppState *st, GError **err) {
    gchar *app = find_app_binary();
    if (!app) {
        g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                    "FUSE binary 'app' not found next to this GUI executable");
        return FALSE;
    }

    g_mkdir_with_parents(st->mount_path, 0755);
    g_mkdir_with_parents(st->storage_path, 0700);

    gchar *argv[] = { app, "-f", st->mount_path, NULL };
    gchar **envp = g_get_environ();
    envp = g_environ_setenv(envp, "DEDUP_DATA", st->storage_path, TRUE);

    GPid pid = 0;
    gboolean ok = g_spawn_async(NULL, argv, envp,
                                G_SPAWN_DO_NOT_REAP_CHILD,
                                NULL, NULL, &pid, err);
    g_strfreev(envp);
    g_free(app);
    if (!ok) return FALSE;

    st->fuse_pid = pid;
    g_child_watch_add(pid, on_child_exit, st);
    return TRUE;
}

static void stop_fuse(AppState *st) {
    if (!st->mount_path) return;
    gchar *argv[] = { "fusermount3", "-u", st->mount_path, NULL };
    GError *err = NULL;
    g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                 NULL, NULL, NULL, NULL, NULL, &err);
    if (err) {
        log_append(st, "[gui] fusermount3 failed: %s", err->message);
        g_error_free(err);
    }
    /* on_child_exit will fire and clean up state */
}

static void open_in_file_manager(const char *path) {
    gchar *uri = g_filename_to_uri(path, NULL, NULL);
    if (!uri) return;
    g_app_info_launch_default_for_uri(uri, NULL, NULL);
    g_free(uri);
}

/* ---------- callbacks ---------- */

static gboolean wait_for_mount_then_open(gpointer user_data) {
    AppState *st = user_data;
    if (st->mounted && st->mount_path) {
        open_in_file_manager(st->mount_path);
    }
    return G_SOURCE_REMOVE;
}

static void on_toggle_clicked(GtkButton *btn, gpointer user_data) {
    AppState *st = user_data;

    if (!st->mounted) {
        gchar *storage = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(st->storage_chooser));
        gchar *mount   = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(st->mount_chooser));
        if (!storage || !mount) {
            status_set(st, "Please pick both folders");
            g_free(storage);
            g_free(mount);
            return;
        }
        g_free(st->storage_path);
        g_free(st->mount_path);
        st->storage_path = storage;
        st->mount_path   = mount;
        config_save(st->storage_path, st->mount_path);

        log_append(st, "[gui] mounting %s -> %s", st->storage_path, st->mount_path);
        GError *err = NULL;
        if (!start_fuse(st, &err)) {
            status_set(st, "Mount failed: %s", err ? err->message : "?");
            log_append(st, "[gui] mount failed: %s", err ? err->message : "?");
            if (err) g_error_free(err);
            return;
        }
        st->mounted = TRUE;
        bookmarks_modify(st->mount_path, TRUE);
        gtk_button_set_label(btn, "Unmount");
        status_set(st, "Mounted at %s", st->mount_path);
        log_append(st, "[gui] mounted (pid=%d)", (int)st->fuse_pid);
        /* let FUSE settle before opening the file manager */
        g_timeout_add(600, wait_for_mount_then_open, st);
    } else {
        log_append(st, "[gui] unmounting %s", st->mount_path);
        stop_fuse(st);
        if (st->mount_path) bookmarks_modify(st->mount_path, FALSE);
        st->mounted = FALSE;
        gtk_button_set_label(btn, "Mount");
        status_set(st, "Unmounted");
    }
}

static void on_window_destroy(GtkWidget *w, gpointer user_data) {
    (void)w;
    AppState *st = user_data;
    if (st->mounted) {
        stop_fuse(st);
        if (st->mount_path) bookmarks_modify(st->mount_path, FALSE);
    }
    gtk_main_quit();
}

/* ---------- UI ---------- */

static GtkWidget *labeled_chooser(const char *label_text, GtkWidget **out_chooser,
                                  const char *initial) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_widget_set_size_request(lbl, 150, -1);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);

    GtkWidget *chooser = gtk_file_chooser_button_new(
        label_text, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    gtk_file_chooser_button_set_width_chars(GTK_FILE_CHOOSER_BUTTON(chooser), 40);
    if (initial && *initial) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), initial);
    }
    *out_chooser = chooser;

    gtk_box_pack_start(GTK_BOX(box), lbl,     FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), chooser, TRUE,  TRUE,  0);
    return box;
}

static void build_ui(AppState *st) {
    st->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(st->window), "deduplication-fs");
    gtk_window_set_default_size(GTK_WINDOW(st->window), 560, 380);
    gtk_container_set_border_width(GTK_CONTAINER(st->window), 16);
    g_signal_connect(st->window, "destroy", G_CALLBACK(on_window_destroy), st);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_add(GTK_CONTAINER(st->window), vbox);

    gchar *cfg_storage = NULL;
    gchar *cfg_mount   = NULL;
    config_load(&cfg_storage, &cfg_mount);

    GtkWidget *row1 = labeled_chooser("Backing storage:", &st->storage_chooser, cfg_storage);
    GtkWidget *row2 = labeled_chooser("Mount point:",     &st->mount_chooser,   cfg_mount);
    gtk_box_pack_start(GTK_BOX(vbox), row1, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), row2, FALSE, FALSE, 0);
    g_free(cfg_storage);
    g_free(cfg_mount);

    st->toggle_btn = gtk_button_new_with_label("Mount");
    g_signal_connect(st->toggle_btn, "clicked", G_CALLBACK(on_toggle_clicked), st);
    gtk_box_pack_start(GTK_BOX(vbox), st->toggle_btn, FALSE, FALSE, 0);

    st->status_label = gtk_label_new("Not mounted");
    gtk_label_set_xalign(GTK_LABEL(st->status_label), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), st->status_label, FALSE, FALSE, 0);

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), sep, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    st->log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(st->log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(st->log_view), TRUE);
    st->log_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(st->log_view));
    gtk_container_add(GTK_CONTAINER(scroll), st->log_view);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    log_append(st, "[gui] ready. Pick a backing storage folder and a mount point.");
}

int main(int argc, char *argv[]) {
    /* avoid zombies if g_child_watch_add can't reap fast enough */
    signal(SIGPIPE, SIG_IGN);

    gtk_init(&argc, &argv);

    AppState st = {0};
    build_ui(&st);
    gtk_widget_show_all(st.window);
    gtk_main();

    g_free(st.storage_path);
    g_free(st.mount_path);
    return 0;
}
