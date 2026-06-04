// Pinback Linux shell: a libadwaita (GTK 4) window hosting a WebKitGTK WebView.
// WebKitGTK links the *system* libwebkitgtk-6.0 and libadwaita-1 (resolved at
// build time via pkg-config), so no browser engine is bundled in the binary —
// libadwaita is a thin, distro-packaged styling layer on top of GTK 4, not a
// vendored engine.
//
// Embedding model (how the shell "loads pinback"):
//   - If PINBACK_URL is set, load it verbatim and spawn nothing (dev/remote).
//   - Otherwise the shell IS the launcher: it spawns the bundled
//     `pinback-server` child on 127.0.0.1:8088, waits for /healthz, then loads
//     it, and kills the child on shutdown. A novice runs one binary, no
//     terminal — the cockpit backend + UI come up underneath it.
//
// Sugar (GNOME 2026, libadwaita 1.9):
//   - AdwApplicationWindow + AdwOverlaySplitView give a native workspace sidebar
//     that collapses to an overlay drawer on narrow widths via an AdwBreakpoint
//     (the GNOME analogue of an adaptive list-detail layout). The content side is
//     an AdwToolbarView: a flat AdwHeaderBar over the WebView, so the chrome
//     blends with the page and the window gets rounded corners for free.
//   - AdwStyleManager follows the system light/dark (and accent) automatically;
//     we mirror the resolved scheme onto the WebView background so there's no
//     white flash in dark mode.
//   - A WebKitUserContentManager script-message handler ("pinback") mirrors the
//     cockpit's workspace list into the native sidebar; activating a row drives
//     window.pinback.selectWorkspace() back in the page. The page posts via
//     window.webkit.messageHandlers.pinback — the same channel the Apple shells
//     use, so the web bridge is shared verbatim.
#include <adwaita.h>
#include <webkit/webkit.h>

#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define PB_HOST "127.0.0.1"
#define PB_PORT 8088

static pid_t g_server_pid = -1;
static GtkListBox *g_sidebar = NULL;   // native workspace switcher
static WebKitWebView *g_web = NULL;
static AdwWindowTitle *g_title = NULL; // content header title/subtitle

// Try one HTTP GET /healthz; return 1 iff the server answers "HTTP/.. 200".
static int health_ok(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PB_PORT);
    inet_pton(AF_INET, PB_HOST, &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return 0; }
    const char *req = "GET /healthz HTTP/1.0\r\nHost: " PB_HOST "\r\n\r\n";
    if (write(fd, req, strlen(req)) < 0) { close(fd); return 0; }
    char buf[64] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n > 12 && memcmp(buf, "HTTP/", 5) == 0 && strstr(buf, " 200") != NULL;
}

// fork+exec pinback-server on the loopback port. PATH-resolved (execvp), with
// a PINBACK_SERVER_BIN override for a bundled/relocated binary.
static void spawn_server(void) {
    const char *bin = g_getenv("PINBACK_SERVER_BIN");
    if (!bin || !*bin) bin = "pinback-server";

    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = { (char *)bin, "--bind", PB_HOST ":8088", "--quiet", NULL };
        execvp(bin, argv);
        _exit(127);                 // exec failed: shell will load anyway
    }
    if (pid > 0) {
        g_server_pid = pid;
        for (int i = 0; i < 150 && !health_ok(); i++)   // up to ~30s
            g_usleep(200 * 1000);
    }
}

static void stop_server(void) {
    if (g_server_pid > 0) {
        kill(g_server_pid, SIGTERM);
        g_server_pid = -1;
    }
}

// ---------------------------------------------------------------------------
// Theme: follow the system light/dark via AdwStyleManager, mirror onto the
// WebView background so the dark cockpit doesn't flash white on load.
// ---------------------------------------------------------------------------
static void apply_scheme(void) {
    if (!g_web) return;
    gboolean dark = adw_style_manager_get_dark(adw_style_manager_get_default());
    GdkRGBA bg;
    gdk_rgba_parse(&bg, dark ? "#0d0f14" : "#ffffff");
    webkit_web_view_set_background_color(g_web, &bg);
}

static void on_dark_changed(AdwStyleManager *sm, GParamSpec *ps, gpointer data) {
    (void)sm; (void)ps; (void)data;
    apply_scheme();
}

// ---------------------------------------------------------------------------
// Native sidebar <-> page bridge.
// ---------------------------------------------------------------------------
static char *jsc_str_prop(JSCValue *obj, const char *name) {
    JSCValue *p = jsc_value_object_get_property(obj, name);
    char *s = NULL;
    if (p && !jsc_value_is_null(p) && !jsc_value_is_undefined(p))
        s = jsc_value_to_string(p);   // newly allocated
    g_clear_object(&p);
    return s;
}

static void clear_listbox(GtkListBox *lb) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(lb))))
        gtk_list_box_remove(lb, child);
}

// web -> native: the cockpit posts {type, activeId, canGoBack, workspaces:[...]}.
static void on_script_message(WebKitUserContentManager *ucm, JSCValue *value, gpointer data) {
    (void)ucm; (void)data;
    if (!g_sidebar || !value || !jsc_value_is_object(value)) return;

    char *active = jsc_str_prop(value, "activeId");

    clear_listbox(g_sidebar);

    JSCValue *arr = jsc_value_object_get_property(value, "workspaces");
    GtkListBoxRow *active_row = NULL;
    if (arr && jsc_value_is_array(arr)) {
        JSCValue *lenv = jsc_value_object_get_property(arr, "length");
        gint32 len = lenv ? jsc_value_to_int32(lenv) : 0;
        g_clear_object(&lenv);
        for (gint32 i = 0; i < len; i++) {
            JSCValue *it = jsc_value_object_get_property_at_index(arr, (guint)i);
            char *id = jsc_str_prop(it, "id");
            char *label = jsc_str_prop(it, "label");
            char *path = jsc_str_prop(it, "path");

            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
            gtk_widget_set_margin_top(box, 4);
            gtk_widget_set_margin_bottom(box, 4);
            gtk_widget_set_margin_start(box, 6);
            gtk_widget_set_margin_end(box, 6);

            GtkWidget *name = gtk_label_new((label && *label) ? label : (id ? id : "(workspace)"));
            gtk_widget_set_halign(name, GTK_ALIGN_START);
            gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
            gtk_box_append(GTK_BOX(box), name);
            if (path && *path) {
                GtkWidget *sub = gtk_label_new(path);
                gtk_widget_set_halign(sub, GTK_ALIGN_START);
                gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_START);
                gtk_widget_add_css_class(sub, "dim-label");
                gtk_widget_add_css_class(sub, "caption");
                gtk_box_append(GTK_BOX(box), sub);
            }

            GtkWidget *row = gtk_list_box_row_new();
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
            if (id) g_object_set_data_full(G_OBJECT(row), "wid", g_strdup(id), g_free);
            gtk_list_box_append(g_sidebar, row);
            if (id && active && strcmp(id, active) == 0)
                active_row = GTK_LIST_BOX_ROW(row);

            g_free(id); g_free(label); g_free(path);
            g_clear_object(&it);
        }
    }
    g_clear_object(&arr);

    if (active_row) gtk_list_box_select_row(g_sidebar, active_row);
    if (g_title) adw_window_title_set_subtitle(g_title, active ? active : "");
    g_free(active);
}

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer data) {
    (void)lb; (void)data;
    const char *id = g_object_get_data(G_OBJECT(row), "wid");
    if (!id || !g_web) return;

    // Build a JS string literal with ' and \ escaped.
    GString *esc = g_string_new(NULL);
    for (const char *c = id; *c; c++) {
        if (*c == '\\' || *c == '\'') g_string_append_c(esc, '\\');
        g_string_append_c(esc, *c);
    }
    char *js = g_strdup_printf(
        "window.pinback&&window.pinback.selectWorkspace('%s')", esc->str);
    webkit_web_view_evaluate_javascript(g_web, js, -1, NULL, NULL, NULL, NULL, NULL);
    g_free(js);
    g_string_free(esc, TRUE);
}

static void on_reload(GtkButton *btn, gpointer web) {
    (void)btn;
    webkit_web_view_reload(WEBKIT_WEB_VIEW(web));
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    GtkWidget *window = adw_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Pinback");
    gtk_window_set_default_size(GTK_WINDOW(window), 1280, 800);

    // WebView + script-message bridge (register before the first load).
    GtkWidget *web = webkit_web_view_new();
    g_web = WEBKIT_WEB_VIEW(web);
    WebKitUserContentManager *ucm =
        webkit_web_view_get_user_content_manager(g_web);
    webkit_user_content_manager_register_script_message_handler(ucm, "pinback", NULL);
    g_signal_connect(ucm, "script-message-received::pinback",
                     G_CALLBACK(on_script_message), NULL);

    // Sidebar: a source-list of workspaces inside its own toolbar view.
    GtkWidget *side_tv = adw_toolbar_view_new();
    GtkWidget *side_hb = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(side_hb),
                                    adw_window_title_new("Workspaces", NULL));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(side_tv), side_hb);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *listbox = gtk_list_box_new();
    g_sidebar = GTK_LIST_BOX(listbox);
    gtk_list_box_set_selection_mode(g_sidebar, GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(listbox, "navigation-sidebar");
    g_signal_connect(listbox, "row-activated", G_CALLBACK(on_row_activated), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), listbox);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(side_tv), scroller);

    // Content: a flat header bar over the WebView.
    GtkWidget *content_tv = adw_toolbar_view_new();
    GtkWidget *hb = adw_header_bar_new();
    g_title = ADW_WINDOW_TITLE(adw_window_title_new("Pinback", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(hb), GTK_WIDGET(g_title));

    GtkWidget *split = adw_overlay_split_view_new();
    GtkWidget *toggle = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(toggle), "sidebar-show-symbolic");
    gtk_widget_set_tooltip_text(toggle, "Toggle workspaces");
    g_object_bind_property(split, "show-sidebar", toggle, "active",
                           G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
    adw_header_bar_pack_start(ADW_HEADER_BAR(hb), toggle);

    GtkWidget *reload = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(reload, "Reload");
    g_signal_connect(reload, "clicked", G_CALLBACK(on_reload), web);
    adw_header_bar_pack_end(ADW_HEADER_BAR(hb), reload);

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(content_tv), hb);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(content_tv), web);

    adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(split), side_tv);
    adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(split), content_tv);
    adw_overlay_split_view_set_max_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(split), 320);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), split);

    // Responsive: collapse the sidebar into an overlay drawer on narrow widths.
    AdwBreakpoint *bp =
        adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 600sp"));
    GValue collapsed = G_VALUE_INIT;
    g_value_init(&collapsed, G_TYPE_BOOLEAN);
    g_value_set_boolean(&collapsed, TRUE);
    adw_breakpoint_add_setter(bp, G_OBJECT(split), "collapsed", &collapsed);
    g_value_unset(&collapsed);
    adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(window), bp);

    // Theme follow.
    g_signal_connect(adw_style_manager_get_default(), "notify::dark",
                     G_CALLBACK(on_dark_changed), NULL);
    apply_scheme();

    const char *url = g_getenv("PINBACK_URL");
    if (!url || !*url) {
        spawn_server();             // self-host the cockpit on loopback
        url = "http://" PB_HOST ":8088";
    }
    webkit_web_view_load_uri(g_web, url);

    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    g_autoptr(AdwApplication) app =
        adw_application_new("dev.pinback.shell", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    stop_server();                  // reap the server when the window closes
    return status;
}
