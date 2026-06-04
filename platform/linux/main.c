// Pinback Linux shell: a GTK 4 window whose entire content is a WebKitGTK
// WebView. WebKitGTK links the *system* libwebkitgtk-6.0 (resolved at build
// time via pkg-config), so no browser engine is bundled in the binary.
#include <gtk/gtk.h>
#include <webkit/webkit.h>

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Pinback");
    gtk_window_set_default_size(GTK_WINDOW(window), 1280, 800);

    GtkWidget *web = webkit_web_view_new();
    const char *url = g_getenv("PINBACK_URL");
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(web),
                             url ? url : "http://127.0.0.1:18192");

    gtk_window_set_child(GTK_WINDOW(window), web);
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
    g_autoptr(GtkApplication) app =
        gtk_application_new("dev.pinback.shell", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    return g_application_run(G_APPLICATION(app), argc, argv);
}
