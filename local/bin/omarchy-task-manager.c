#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

enum {
    COL_PID = 0,
    COL_NAME,
    COL_CPU,
    COL_MEM,
    COL_USER,
    COL_TYPE,
    COL_VISIBLE,
    NUM_COLS
};

typedef struct {
    GtkWidget *window;
    GtkWidget *tree_view;
    GtkListStore *store;
    GtkTreeModelFilter *filter;
    GtkWidget *search_entry;
    GtkWidget *filter_combo;
    GtkWidget *lbl_summary;
    GtkWidget *btn_kill;
    guint timer_id;
    gchar *filter_text;
    gint filter_type; /* 0: all, 1: apps only (u0_a), 2: system */
} TaskManagerApp;

static TaskManagerApp app;

/* Helper to trim leading/trailing whitespace */
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

/* Format CPU/MEM for display */
static void cpu_cell_data_func(GtkTreeViewColumn *col, GtkCellRenderer *renderer,
                               GtkTreeModel *model, GtkTreeIter *iter, gpointer data) {
    gfloat cpu;
    gtk_tree_model_get(model, iter, COL_CPU, &cpu, -1);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f %%", cpu);
    g_object_set(renderer, "text", buf, NULL);
}

static void mem_cell_data_func(GtkTreeViewColumn *col, GtkCellRenderer *renderer,
                               GtkTreeModel *model, GtkTreeIter *iter, gpointer data) {
    gfloat mem;
    gtk_tree_model_get(model, iter, COL_MEM, &mem, -1);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f %%", mem);
    g_object_set(renderer, "text", buf, NULL);
}

/* Filter function */
static gboolean row_visible_func(GtkTreeModel *model, GtkTreeIter *iter, gpointer data) {
    gchar *name = NULL;
    gchar *user = NULL;
    gint pid = 0;
    gtk_tree_model_get(model, iter,
                       COL_PID, &pid,
                       COL_NAME, &name,
                       COL_USER, &user,
                       -1);

    gboolean visible = TRUE;

    /* Category filter */
    if (app.filter_type == 1) { /* Apps only */
        if (!user || strncmp(user, "u0_a", 4) != 0) {
            visible = FALSE;
        }
    } else if (app.filter_type == 2) { /* System */
        if (user && strncmp(user, "u0_a", 4) == 0) {
            visible = FALSE;
        }
    }

    /* Text search filter */
    if (visible && app.filter_text && app.filter_text[0] != '\0') {
        char pid_str[32];
        snprintf(pid_str, sizeof(pid_str), "%d", pid);

        gchar *down_name = name ? g_utf8_strdown(name, -1) : NULL;
        gchar *down_search = g_utf8_strdown(app.filter_text, -1);

        if ((!down_name || !strstr(down_name, down_search)) && !strstr(pid_str, down_search)) {
            visible = FALSE;
        }

        g_free(down_name);
        g_free(down_search);
    }

    g_free(name);
    g_free(user);
    return visible;
}

/* Read /proc/meminfo for system memory */
static void get_memory_info(long *total_kb, long *avail_kb) {
    *total_kb = 0;
    *avail_kb = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            sscanf(line + 9, "%ld", total_kb);
        } else if (strncmp(line, "MemAvailable:", 13) == 0) {
            sscanf(line + 13, "%ld", avail_kb);
        }
    }
    fclose(f);
}

/* Refresh processes via droid ps -A */
static void refresh_processes(void) {
    /* Preserve selection if possible */
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.tree_view));
    GtkTreeModel *model = NULL;
    GtkTreeIter selected_iter;
    gint sel_pid = -1;

    if (gtk_tree_selection_get_selected(sel, &model, &selected_iter)) {
        gtk_tree_model_get(model, &selected_iter, COL_PID, &sel_pid, -1);
    }

    FILE *pipe = popen("droid ps -A -o PID,USER,%CPU,%MEM,NAME:80 2>/dev/null", "r");
    if (!pipe) return;

    gtk_list_store_clear(app.store);

    char line[512];
    int line_num = 0;
    int total_tasks = 0;
    int app_tasks = 0;

    while (fgets(line, sizeof(line), pipe)) {
        line_num++;
        if (line_num == 1) continue; /* Header line */

        int pid = 0;
        char user[64] = {0};
        float cpu = 0.0f;
        float mem = 0.0f;
        char name[256] = {0};

        if (sscanf(line, "%d %63s %f %f %255s", &pid, user, &cpu, &mem, name) >= 5) {
            total_tasks++;
            const char *type = "System";
            if (strncmp(user, "u0_a", 4) == 0) {
                type = "Android app";
                app_tasks++;
            } else if (name[0] == '[') {
                type = "Kernel";
            }

            GtkTreeIter iter;
            gtk_list_store_append(app.store, &iter);
            gtk_list_store_set(app.store, &iter,
                               COL_PID, pid,
                               COL_NAME, name,
                               COL_CPU, cpu,
                               COL_MEM, mem,
                               COL_USER, user,
                               COL_TYPE, type,
                               -1);

            if (pid == sel_pid) {
                /* Will restore selection after */
            }
        }
    }
    pclose(pipe);

    /* Update summary */
    long mem_total = 0, mem_avail = 0;
    get_memory_info(&mem_total, &mem_avail);
    long mem_used = mem_total - mem_avail;
    double used_gb = mem_used / 1048576.0;
    double tot_gb = mem_total / 1048576.0;
    int pct = mem_total > 0 ? (int)((mem_used * 100) / mem_total) : 0;

    char summary_text[256];
    snprintf(summary_text, sizeof(summary_text),
             "󰍛 <b>Tasks:</b> %d running (%d apps)   │    <b>RAM:</b> %.1f / %.1f GB (%d%% used)",
             total_tasks, app_tasks, used_gb, tot_gb, pct);
    gtk_label_set_markup(GTK_LABEL(app.lbl_summary), summary_text);

    gtk_tree_model_filter_refilter(app.filter);
}

static gboolean on_timer_tick(gpointer data) {
    refresh_processes();
    return G_SOURCE_CONTINUE;
}

static void on_search_changed(GtkSearchEntry *entry, gpointer data) {
    g_free(app.filter_text);
    app.filter_text = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
    gtk_tree_model_filter_refilter(app.filter);
}

static void on_combo_changed(GtkComboBox *combo, gpointer data) {
    app.filter_type = gtk_combo_box_get_active(combo);
    gtk_tree_model_filter_refilter(app.filter);
}

static void kill_selected_task(void) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.tree_view));
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;

    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) {
        return;
    }

    gint pid = 0;
    gchar *name = NULL;
    gchar *user = NULL;
    gtk_tree_model_get(model, &iter,
                       COL_PID, &pid,
                       COL_NAME, &name,
                       COL_USER, &user,
                       -1);

    if (!name || pid <= 0) {
        g_free(name);
        g_free(user);
        return;
    }

    /* Confirmation dialog */
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app.window),
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_WARNING,
                                               GTK_BUTTONS_OK_CANCEL,
                                               "End this process?");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                            "Process: %s\nPID: %d\nUser: %s",
                                            name, pid, user);

    gint resp = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (resp == GTK_RESPONSE_OK) {
        char cmd[512];
        if (strncmp(user, "u0_a", 4) == 0 && strchr(name, '.')) {
            /* Android application: use am force-stop */
            snprintf(cmd, sizeof(cmd), "droid am force-stop %s 2>/dev/null", name);
            system(cmd);
        }
        /* Fallback / general kill */
        snprintf(cmd, sizeof(cmd), "droid kill -9 %d 2>/dev/null", pid);
        system(cmd);

        usleep(150000); /* 150ms wait */
        refresh_processes();
    }

    g_free(name);
    g_free(user);
}

static void on_kill_clicked(GtkButton *btn, gpointer data) {
    kill_selected_task();
}

static void on_refresh_clicked(GtkButton *btn, gpointer data) {
    refresh_processes();
}

static void on_row_activated(GtkTreeView *tree_view, GtkTreePath *path,
                             GtkTreeViewColumn *column, gpointer data) {
    kill_selected_task();
}

int main(int argc, char **argv) {
    g_set_prgname("omarchy-task-manager");
    g_set_application_name("Android task manager");
    gtk_init(&argc, &argv);

    memset(&app, 0, sizeof(app));
    app.filter_text = g_strdup("");
    app.filter_type = 0;

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Android task manager");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 820, 540);
    gtk_window_set_position(GTK_WINDOW(app.window), GTK_WIN_POS_CENTER);
    g_object_set(app.window, "role", "omarchy.taskmanager", NULL);

    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    /* Main vertical container */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app.window), vbox);

    /* Header Bar */
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Task manager");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header), "Android processes and resources");
    gtk_window_set_titlebar(GTK_WINDOW(app.window), header);

    /* Search entry in header */
    app.search_entry = gtk_search_entry_new();
    gtk_widget_set_size_request(app.search_entry, 180, -1);
    g_signal_connect(app.search_entry, "search-changed", G_CALLBACK(on_search_changed), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), app.search_entry);

    /* Combo filter */
    app.filter_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.filter_combo), "All");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.filter_combo), "Apps only (u0_a*)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.filter_combo), "System only");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.filter_combo), 0);
    g_signal_connect(app.filter_combo, "changed", G_CALLBACK(on_combo_changed), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), app.filter_combo);

    /* Refresh button */
    GtkWidget *btn_refresh = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(btn_refresh, "Refresh the process list");
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), btn_refresh);

    /* End task button */
    app.btn_kill = gtk_button_new_with_label("End task");
    GtkStyleContext *style_ctx = gtk_widget_get_style_context(app.btn_kill);
    gtk_style_context_add_class(style_ctx, "destructive-action");
    gtk_widget_set_tooltip_text(app.btn_kill, "Force-stop the selected app or process");
    g_signal_connect(app.btn_kill, "clicked", G_CALLBACK(on_kill_clicked), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), app.btn_kill);

    /* Status / Summary Info Bar */
    GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(info_box), 8);
    app.lbl_summary = gtk_label_new("Loading task and RAM figures...");
    gtk_label_set_use_markup(GTK_LABEL(app.lbl_summary), TRUE);
    gtk_box_pack_start(GTK_BOX(info_box), app.lbl_summary, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(vbox), info_box, FALSE, FALSE, 0);

    /* List Store & Model Filter */
    app.store = gtk_list_store_new(NUM_COLS,
                                  G_TYPE_INT,       /* PID */
                                  G_TYPE_STRING,    /* Name */
                                  G_TYPE_FLOAT,     /* CPU */
                                  G_TYPE_FLOAT,     /* MEM */
                                  G_TYPE_STRING,    /* User */
                                  G_TYPE_STRING,    /* Type */
                                  G_TYPE_BOOLEAN);  /* Visible */

    app.filter = GTK_TREE_MODEL_FILTER(gtk_tree_model_filter_new(GTK_TREE_MODEL(app.store), NULL));
    gtk_tree_model_filter_set_visible_func(app.filter, row_visible_func, NULL, NULL);

    /* Tree View */
    app.tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app.filter));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(app.tree_view), TRUE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(app.tree_view), FALSE);
    g_signal_connect(app.tree_view, "row-activated", G_CALLBACK(on_row_activated), NULL);

    /* Columns */
    GtkCellRenderer *renderer_text;
    GtkTreeViewColumn *col;

    /* PID */
    renderer_text = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("PID", renderer_text, "text", COL_PID, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_PID);
    gtk_tree_view_column_set_min_width(col, 70);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app.tree_view), col);

    /* Name */
    renderer_text = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("App / process", renderer_text, "text", COL_NAME, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_NAME);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app.tree_view), col);

    /* CPU % */
    renderer_text = gtk_cell_renderer_text_new();
    g_object_set(renderer_text, "xalign", 1.0, NULL);
    col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col, "CPU");
    gtk_tree_view_column_pack_start(col, renderer_text, TRUE);
    gtk_tree_view_column_set_cell_data_func(col, renderer_text, cpu_cell_data_func, NULL, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_CPU);
    gtk_tree_view_column_set_min_width(col, 80);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app.tree_view), col);

    /* MEM % */
    renderer_text = gtk_cell_renderer_text_new();
    g_object_set(renderer_text, "xalign", 1.0, NULL);
    col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col, "Memory");
    gtk_tree_view_column_pack_start(col, renderer_text, TRUE);
    gtk_tree_view_column_set_cell_data_func(col, renderer_text, mem_cell_data_func, NULL, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_MEM);
    gtk_tree_view_column_set_min_width(col, 90);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app.tree_view), col);

    /* User */
    renderer_text = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("User", renderer_text, "text", COL_USER, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_USER);
    gtk_tree_view_column_set_min_width(col, 90);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app.tree_view), col);

    /* Type */
    renderer_text = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("Type", renderer_text, "text", COL_TYPE, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_TYPE);
    gtk_tree_view_column_set_min_width(col, 100);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app.tree_view), col);

    /* Scrolled Window */
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_POLICY_AUTOMATIC,
                                  GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), app.tree_view);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    /* Populate initially */
    refresh_processes();

    /* Timer for periodic refresh (every 3 seconds) */
    app.timer_id = g_timeout_add_seconds(3, on_timer_tick, NULL);

    gtk_widget_show_all(app.window);
    gtk_main();

    if (app.timer_id > 0) {
        g_source_remove(app.timer_id);
    }
    g_free(app.filter_text);

    return 0;
}
