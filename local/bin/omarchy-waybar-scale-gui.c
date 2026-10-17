#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

typedef struct {
    GtkWidget *window;
    GtkWidget *label_val;
    GtkWidget *scale_slider;
    GtkWidget *check_live;
    double initial_scale;
    double current_scale;
    guint live_timeout_id;
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

static double read_current_scale(void) {
    char path[512];
    const char *home = getenv("HOME") ? getenv("HOME") : "/root";
    snprintf(path, sizeof(path), "%s/.config/waybar-themes/.scale", home);
    FILE *f = fopen(path, "r");
    if (!f) return 1.40;
    double val = 1.40;
    if (fscanf(f, "%lf", &val) == 1) {
        if (val < 0.5) val = 0.5;
        if (val > 3.0) val = 3.0;
    }
    fclose(f);
    return val;
}

static void apply_scale_command(double val) {
    char cmd[512];
    const char *home = getenv("HOME") ? getenv("HOME") : "/root";
    snprintf(cmd, sizeof(cmd), "%s/.local/bin/omarchy-waybar-scale set %.2f >/dev/null 2>&1 &", home, val);
    system(cmd);
}

static gboolean on_live_debounce(gpointer user_data) {
    AppState *app = (AppState *)user_data;
    apply_scale_command(app->current_scale);
    app->live_timeout_id = 0;
    return G_SOURCE_REMOVE;
}

static void update_scale_ui(AppState *app, double val) {
    app->current_scale = val;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2fx  (%d%%)", val, (int)round(val * 100.0));
    gtk_label_set_text(GTK_LABEL(app->label_val), buf);

    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->check_live))) {
        if (app->live_timeout_id != 0) {
            g_source_remove(app->live_timeout_id);
        }
        app->live_timeout_id = g_timeout_add(250, on_live_debounce, app);
    }
}

static void on_slider_changed(GtkRange *range, gpointer user_data) {
    AppState *app = (AppState *)user_data;
    double val = gtk_range_get_value(range);
    update_scale_ui(app, val);
}

static void on_preset_clicked(GtkButton *btn, gpointer user_data) {
    AppState *app = (AppState *)user_data;
    const char *txt = gtk_button_get_label(btn);
    double target = 1.0;
    if (strstr(txt, "1.0")) target = 1.0;
    else if (strstr(txt, "1.25")) target = 1.25;
    else if (strstr(txt, "1.40")) target = 1.40;
    else if (strstr(txt, "1.60")) target = 1.60;
    else if (strstr(txt, "1.80")) target = 1.80;

    gtk_range_set_value(GTK_RANGE(app->scale_slider), target);
}

static void on_save_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppState *app = (AppState *)user_data;
    if (app->live_timeout_id != 0) {
        g_source_remove(app->live_timeout_id);
        app->live_timeout_id = 0;
    }
    char cmd[512];
    const char *home = getenv("HOME") ? getenv("HOME") : "/root";
    snprintf(cmd, sizeof(cmd), "%s/.local/bin/omarchy-waybar-scale set %.2f", home, app->current_scale);
    system(cmd);
    gtk_widget_destroy(app->window);
}

static void on_cancel_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppState *app = (AppState *)user_data;
    if (app->live_timeout_id != 0) {
        g_source_remove(app->live_timeout_id);
        app->live_timeout_id = 0;
    }
    // If the live preview changed the scale, undo it
    if (fabs(app->current_scale - app->initial_scale) > 0.01) {
        apply_scale_command(app->initial_scale);
    }
    gtk_widget_destroy(app->window);
}

static void apply_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    const char *css = 
        "window { background-color: #1a1b26; color: #c0caf5; font-family: 'JetBrainsMono Nerd Font', 'SF Pro Display', sans-serif; }\n"
        ".card { background-color: #13141c; border: 1px solid rgba(122, 162, 247, 0.2); border-radius: 12px; padding: 14px; margin: 6px 0; }\n"
        ".title { font-size: 16px; font-weight: bold; color: #7aa2f7; }\n"
        ".subtitle { font-size: 11px; color: #565f89; }\n"
        ".scale-val { font-size: 26px; font-weight: 800; color: #7aa2f7; }\n"
        "scale trough { background-color: #24283b; border-radius: 8px; min-height: 10px; }\n"
        "scale highlight { background-color: #7aa2f7; border-radius: 8px; min-height: 10px; }\n"
        "scale slider { background-color: #bb9af7; border-radius: 50%; min-width: 22px; min-height: 22px; }\n"
        "scale slider:hover { background-color: #7aa2f7; }\n"
        "button { background-color: #24283b; color: #c0caf5; border: 1px solid #414868; border-radius: 8px; padding: 6px 14px; font-weight: bold; font-size: 12px; }\n"
        "button:hover { background-color: #414868; color: #ffffff; }\n"
        "button.primary { background-color: #7aa2f7; color: #13141c; border: none; }\n"
        "button.primary:hover { background-color: #89b4fa; color: #000000; }\n"
        "button.preset { padding: 4px 10px; font-size: 11px; min-width: 48px; }\n"
        "checkbutton label { font-size: 11px; color: #a9b1d6; }\n";

    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(provider);
}

int main(int argc, char *argv[]) {
    ensure_wayland_display();
    g_set_prgname("omarchy-waybar-scale");
    g_set_application_name("Waybar scale settings");
    gtk_init(&argc, &argv);

    apply_css();

    AppState app;
    memset(&app, 0, sizeof(app));
    app.initial_scale = read_current_scale();
    app.current_scale = app.initial_scale;

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Scale settings — Waybar");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 460, 390);
    gtk_window_set_position(GTK_WINDOW(app.window), GTK_WIN_POS_CENTER);
    gtk_container_set_border_width(GTK_CONTAINER(app.window), 18);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(app.window), root_box);

    // Header
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *lbl_title = gtk_label_new("󰏘 Waybar scale");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_title), "title");
    gtk_widget_set_halign(lbl_title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(header_box), lbl_title, FALSE, FALSE, 0);

    GtkWidget *lbl_sub = gtk_label_new("Scales the height, fonts and icons of every theme together.");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_sub), "subtitle");
    gtk_widget_set_halign(lbl_sub, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(header_box), lbl_sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root_box), header_box, FALSE, FALSE, 0);

    // A card with the big value and a slider
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "card");

    char val_buf[64];
    snprintf(val_buf, sizeof(val_buf), "%.2fx  (%d%%)", app.current_scale, (int)round(app.current_scale * 100.0));
    app.label_val = gtk_label_new(val_buf);
    gtk_style_context_add_class(gtk_widget_get_style_context(app.label_val), "scale-val");
    gtk_box_pack_start(GTK_BOX(card), app.label_val, FALSE, FALSE, 0);

    // Slider
    app.scale_slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.70, 2.20, 0.05);
    gtk_range_set_value(GTK_RANGE(app.scale_slider), app.current_scale);
    gtk_scale_add_mark(GTK_SCALE(app.scale_slider), 1.00, GTK_POS_BOTTOM, "1.0x");
    gtk_scale_add_mark(GTK_SCALE(app.scale_slider), 1.40, GTK_POS_BOTTOM, "1.4x");
    gtk_scale_add_mark(GTK_SCALE(app.scale_slider), 1.80, GTK_POS_BOTTOM, "1.8x");
    gtk_scale_set_draw_value(GTK_SCALE(app.scale_slider), FALSE);
    g_signal_connect(app.scale_slider, "value-changed", G_CALLBACK(on_slider_changed), &app);
    gtk_box_pack_start(GTK_BOX(card), app.scale_slider, FALSE, FALSE, 4);

    // Presets
    GtkWidget *presets_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(presets_box, GTK_ALIGN_CENTER);
    const char *presets[] = { "1.0x", "1.25x", "1.40x", "1.60x", "1.80x" };
    for (int i = 0; i < 5; i++) {
        GtkWidget *pbtn = gtk_button_new_with_label(presets[i]);
        gtk_style_context_add_class(gtk_widget_get_style_context(pbtn), "preset");
        g_signal_connect(pbtn, "clicked", G_CALLBACK(on_preset_clicked), &app);
        gtk_box_pack_start(GTK_BOX(presets_box), pbtn, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(card), presets_box, FALSE, FALSE, 2);

    gtk_box_pack_start(GTK_BOX(root_box), card, FALSE, FALSE, 0);

    // Live preview checkbox
    app.check_live = gtk_check_button_new_with_label("Preview changes live on Waybar");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.check_live), FALSE);
    gtk_box_pack_start(GTK_BOX(root_box), app.check_live, FALSE, FALSE, 0);

    // Action buttons at the bottom
    GtkWidget *action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(action_box, GTK_ALIGN_END);

    GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
    g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_cancel_clicked), &app);
    gtk_box_pack_start(GTK_BOX(action_box), btn_cancel, FALSE, FALSE, 0);

    GtkWidget *btn_save = gtk_button_new_with_label("✔ Save and apply");
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_save), "primary");
    g_signal_connect(btn_save, "clicked", G_CALLBACK(on_save_clicked), &app);
    gtk_box_pack_start(GTK_BOX(action_box), btn_save, FALSE, FALSE, 0);

    gtk_box_pack_end(GTK_BOX(root_box), action_box, FALSE, FALSE, 4);

    gtk_widget_show_all(app.window);
    gtk_main();

    return 0;
}
