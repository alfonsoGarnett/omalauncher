#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

typedef struct {
    GtkWidget *window;
    GtkWidget *slider_pointer;
    GtkWidget *slider_scroll;
    GtkWidget *label_pointer_val;
    GtkWidget *label_scroll_val;
    GtkWidget *switch_natural;
    GtkWidget *label_natural_status;
    double pointer_speed;
    double scroll_speed;
    int natural_scroll;
    guint debounce_timer;
} AppState;

static void ensure_wayland_display(void) {
    if (!getenv("WAYLAND_DISPLAY")) {
        if (access("/run/user/0/wayland-1", F_OK) == 0) {
            setenv("WAYLAND_DISPLAY", "wayland-1", 1);
        } else if (access("/run/user/0/wayland-0", F_OK) == 0) {
            setenv("WAYLAND_DISPLAY", "wayland-0", 1);
        }
    }
    setenv("GDK_BACKEND", "wayland", 1);
}

static void read_mouse_config(AppState *app) {
    app->pointer_speed = 1.0;
    app->scroll_speed = 1.0;
    app->natural_scroll = 0;

    const char *home = getenv("HOME") ? getenv("HOME") : "/root";
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/omalauncher/mouse.conf", home);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64];
        float val;
        if (sscanf(line, "%63[^=]=%f", key, &val) == 2) {
            // Trim whitespace
            char *k = key;
            while (*k == ' ') k++;
            char *end = k + strlen(k) - 1;
            while (end > k && (*end == ' ' || *end == '\r' || *end == '\n')) *end-- = '\0';

            if (strcmp(k, "pointer_speed") == 0) {
                app->pointer_speed = val;
            } else if (strcmp(k, "scroll_speed") == 0) {
                app->scroll_speed = val;
            } else if (strcmp(k, "natural_scroll") == 0) {
                app->natural_scroll = (int)val;
            }
        }
    }
    fclose(f);
}

static void apply_mouse_set(const char *key, double val) {
    char cmd[256];
    if (strcmp(key, "natural_scroll") == 0) {
        snprintf(cmd, sizeof(cmd), "mouse-set %s %d >/dev/null 2>&1 &", key, (int)val);
    } else {
        snprintf(cmd, sizeof(cmd), "mouse-set %s %.1f >/dev/null 2>&1 &", key, val);
    }
    system(cmd);
}

static void on_pointer_slider_changed(GtkRange *range, gpointer user_data) {
    AppState *app = (AppState *)user_data;
    double val = gtk_range_get_value(range);
    app->pointer_speed = val;

    char buf[32];
    snprintf(buf, sizeof(buf), "%.1fx", val);
    gtk_label_set_text(GTK_LABEL(app->label_pointer_val), buf);

    apply_mouse_set("pointer_speed", val);
}

static void on_scroll_slider_changed(GtkRange *range, gpointer user_data) {
    AppState *app = (AppState *)user_data;
    double val = gtk_range_get_value(range);
    app->scroll_speed = val;

    char buf[32];
    snprintf(buf, sizeof(buf), "%.1fx", val);
    gtk_label_set_text(GTK_LABEL(app->label_scroll_val), buf);

    apply_mouse_set("scroll_speed", val);
}

static void on_natural_switch_activated(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    AppState *app = (AppState *)user_data;
    gboolean active = gtk_switch_get_active(GTK_SWITCH(app->switch_natural));
    app->natural_scroll = active ? 1 : 0;

    if (active) {
        gtk_label_set_text(GTK_LABEL(app->label_natural_status), "󰁝 On (reversed, like a Mac or touchpad)");
    } else {
        gtk_label_set_text(GTK_LABEL(app->label_natural_status), "󰁅 Off (standard PC direction)");
    }

    apply_mouse_set("natural_scroll", app->natural_scroll);
}

static void on_preset_clicked(GtkButton *btn, gpointer user_data) {
    AppState *app = (AppState *)user_data;
    const char *label = gtk_button_get_label(btn);
    double ptr = 1.0, scrl = 1.0;

    if (strstr(label, "0.8x")) { ptr = 0.8; scrl = 0.8; }
    else if (strstr(label, "1.0x")) { ptr = 1.0; scrl = 1.0; }
    else if (strstr(label, "1.5x")) { ptr = 1.5; scrl = 1.5; }
    else if (strstr(label, "2.0x")) { ptr = 2.0; scrl = 2.0; }

    gtk_range_set_value(GTK_RANGE(app->slider_pointer), ptr);
    gtk_range_set_value(GTK_RANGE(app->slider_scroll), scrl);
}

static void on_reset_clicked(GtkButton *btn, gpointer user_data) {
    AppState *app = (AppState *)user_data;
    gtk_range_set_value(GTK_RANGE(app->slider_pointer), 1.0);
    gtk_range_set_value(GTK_RANGE(app->slider_scroll), 1.0);
    gtk_switch_set_active(GTK_SWITCH(app->switch_natural), FALSE);
}

static void on_close_clicked(GtkButton *btn, gpointer user_data) {
    AppState *app = (AppState *)user_data;
    gtk_widget_destroy(app->window);
}

int main(int argc, char *argv[]) {
    ensure_wayland_display();
    gtk_init(&argc, &argv);

    AppState app;
    memset(&app, 0, sizeof(app));
    read_mouse_config(&app);

    // CSS Styling Tokyo Night
    const char *css =
        "* {"
        "  font-family: 'JetBrains Mono', 'Symbols Nerd Font Mono', monospace;"
        "}"
        "window {"
        "  background-color: #1a1b26;"
        "  color: #c0caf5;"
        "  border: 2px solid #7aa2f7;"
        "}"
        ".card {"
        "  background-color: #16161e;"
        "  border: 1px solid #292e42;"
        "  border-radius: 8px;"
        "  padding: 14px 16px;"
        "}"
        ".header-title {"
        "  font-size: 15px;"
        "  font-weight: bold;"
        "  color: #7aa2f7;"
        "}"
        ".header-sub {"
        "  font-size: 11px;"
        "  color: #9ece6a;"
        "}"
        ".section-lbl {"
        "  font-size: 12px;"
        "  font-weight: bold;"
        "  color: #e0af68;"
        "}"
        ".val-badge {"
        "  font-size: 13px;"
        "  font-weight: bold;"
        "  color: #7dcfff;"
        "}"
        "scale highlight {"
        "  background-color: #7aa2f7;"
        "  border-radius: 4px;"
        "}"
        "scale trough {"
        "  background-color: #24283b;"
        "  border-radius: 4px;"
        "}"
        "scale.cyan highlight {"
        "  background-color: #7dcfff;"
        "}"
        "button.preset-btn {"
        "  background-color: #24283b;"
        "  color: #c0caf5;"
        "  border: 1px solid #414868;"
        "  border-radius: 6px;"
        "  padding: 6px 12px;"
        "  font-size: 11px;"
        "}"
        "button.preset-btn:hover {"
        "  background-color: #7aa2f7;"
        "  color: #1a1b26;"
        "}"
        "button.reset-btn {"
        "  background-color: #3b4261;"
        "  color: #e0af68;"
        "  border: 1px solid #e0af68;"
        "  border-radius: 6px;"
        "  padding: 6px 14px;"
        "  font-weight: bold;"
        "}"
        "button.reset-btn:hover {"
        "  background-color: #e0af68;"
        "  color: #16161e;"
        "}"
        "button.close-btn {"
        "  background-color: #f7768e;"
        "  color: #1a1b26;"
        "  border: none;"
        "  border-radius: 6px;"
        "  padding: 6px 16px;"
        "  font-weight: bold;"
        "}"
        "button.close-btn:hover {"
        "  background-color: #ff9e64;"
        "}";

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "󰍽 Mouse & scroll settings");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 540, 520);
    gtk_window_set_position(GTK_WINDOW(app.window), GTK_WIN_POS_CENTER);
    gtk_container_set_border_width(GTK_CONTAINER(app.window), 16);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_add(GTK_CONTAINER(app.window), main_box);

    // Header
    GtkWidget *head_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(head_box), "card");

    GtkWidget *lbl_title = gtk_label_new("󰍽  MOUSE & SCROLL // OMARCHY DEX");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_title), "header-title");
    gtk_widget_set_halign(lbl_title, GTK_ALIGN_START);

    GtkWidget *lbl_sub = gtk_label_new("● Live settings for pointer capture (Ctrl+Alt+M) and Samsung DeX");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_sub), "header-sub");
    gtk_widget_set_halign(lbl_sub, GTK_ALIGN_START);

    gtk_box_pack_start(GTK_BOX(head_box), lbl_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head_box), lbl_sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main_box), head_box, FALSE, FALSE, 0);

    // Card 1: pointer
    GtkWidget *card_ptr = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(card_ptr), "card");

    GtkWidget *row_ptr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *lbl_ptr = gtk_label_new("󰍽  Pointer sensitivity:");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_ptr), "section-lbl");
    gtk_widget_set_halign(lbl_ptr, GTK_ALIGN_START);

    char buf[32];
    snprintf(buf, sizeof(buf), "%.1fx", app.pointer_speed);
    app.label_pointer_val = gtk_label_new(buf);
    gtk_style_context_add_class(gtk_widget_get_style_context(app.label_pointer_val), "val-badge");
    gtk_widget_set_halign(app.label_pointer_val, GTK_ALIGN_END);

    gtk_box_pack_start(GTK_BOX(row_ptr), lbl_ptr, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(row_ptr), app.label_pointer_val, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card_ptr), row_ptr, FALSE, FALSE, 0);

    app.slider_pointer = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.2, 3.0, 0.1);
    gtk_range_set_value(GTK_RANGE(app.slider_pointer), app.pointer_speed);
    gtk_scale_set_draw_value(GTK_SCALE(app.slider_pointer), FALSE);
    g_signal_connect(app.slider_pointer, "value-changed", G_CALLBACK(on_pointer_slider_changed), &app);
    gtk_box_pack_start(GTK_BOX(card_ptr), app.slider_pointer, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(main_box), card_ptr, FALSE, FALSE, 0);

    // Card 2: scroll wheel
    GtkWidget *card_scrl = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(card_scrl), "card");

    GtkWidget *row_scrl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *lbl_scrl = gtk_label_new("󰁝  Scroll wheel speed:");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_scrl), "section-lbl");
    gtk_widget_set_halign(lbl_scrl, GTK_ALIGN_START);

    snprintf(buf, sizeof(buf), "%.1fx", app.scroll_speed);
    app.label_scroll_val = gtk_label_new(buf);
    gtk_style_context_add_class(gtk_widget_get_style_context(app.label_scroll_val), "val-badge");
    gtk_widget_set_halign(app.label_scroll_val, GTK_ALIGN_END);

    gtk_box_pack_start(GTK_BOX(row_scrl), lbl_scrl, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(row_scrl), app.label_scroll_val, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card_scrl), row_scrl, FALSE, FALSE, 0);

    app.slider_scroll = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.2, 4.0, 0.1);
    gtk_range_set_value(GTK_RANGE(app.slider_scroll), app.scroll_speed);
    gtk_scale_set_draw_value(GTK_SCALE(app.slider_scroll), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(app.slider_scroll), "cyan");
    g_signal_connect(app.slider_scroll, "value-changed", G_CALLBACK(on_scroll_slider_changed), &app);
    gtk_box_pack_start(GTK_BOX(card_scrl), app.slider_scroll, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(main_box), card_scrl, FALSE, FALSE, 0);

    // Card 3: natural scrolling
    GtkWidget *card_nat = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(card_nat), "card");

    GtkWidget *row_nat = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *lbl_nat_title = gtk_label_new("󰌑  Natural scrolling (reverse the direction):");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_nat_title), "section-lbl");
    gtk_widget_set_halign(lbl_nat_title, GTK_ALIGN_START);

    app.switch_natural = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(app.switch_natural), app.natural_scroll == 1);
    g_signal_connect(app.switch_natural, "notify::active", G_CALLBACK(on_natural_switch_activated), &app);
    gtk_widget_set_halign(app.switch_natural, GTK_ALIGN_END);

    gtk_box_pack_start(GTK_BOX(row_nat), lbl_nat_title, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(row_nat), app.switch_natural, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card_nat), row_nat, FALSE, FALSE, 0);

    const char *nat_text = (app.natural_scroll == 1)
        ? "󰁝 On (reversed, like a Mac or touchpad)"
        : "󰁅 Off (standard PC direction)";
    app.label_natural_status = gtk_label_new(nat_text);
    gtk_style_context_add_class(gtk_widget_get_style_context(app.label_natural_status), "header-sub");
    gtk_widget_set_halign(app.label_natural_status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(card_nat), app.label_natural_status, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(main_box), card_nat, FALSE, FALSE, 0);

    // Quick presets row
    GtkWidget *row_presets = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    const char *presets[] = { "0.8x Slow", "1.0x Normal", "1.5x Fast", "2.0x Ultra" };
    for (int i = 0; i < 4; i++) {
        GtkWidget *pbtn = gtk_button_new_with_label(presets[i]);
        gtk_style_context_add_class(gtk_widget_get_style_context(pbtn), "preset-btn");
        g_signal_connect(pbtn, "clicked", G_CALLBACK(on_preset_clicked), &app);
        gtk_box_pack_start(GTK_BOX(row_presets), pbtn, TRUE, TRUE, 0);
    }
    gtk_box_pack_start(GTK_BOX(main_box), row_presets, FALSE, FALSE, 0);

    // Action buttons at the bottom
    GtkWidget *action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(action_box, GTK_ALIGN_END);

    GtkWidget *btn_reset = gtk_button_new_with_label("  Reset to 1.0x");
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_reset), "reset-btn");
    g_signal_connect(btn_reset, "clicked", G_CALLBACK(on_reset_clicked), &app);

    GtkWidget *btn_close = gtk_button_new_with_label("✕  Close");
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_close), "close-btn");
    g_signal_connect(btn_close, "clicked", G_CALLBACK(on_close_clicked), &app);

    gtk_box_pack_start(GTK_BOX(action_box), btn_reset, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(action_box), btn_close, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(main_box), action_box, FALSE, FALSE, 0);

    gtk_widget_show_all(app.window);
    gtk_main();

    return 0;
}
