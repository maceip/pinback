// Pinback Linux shell: a GTK 4 window whose entire content is a WebKitGTK
// WebView. WebKitGTK links the *system* libwebkitgtk-6.0 (resolved at build
// time via pkg-config), so no browser engine is bundled in the binary.
//
// Embedding model (how the shell "loads pinback"):
//   - If PINBACK_URL is set, load it verbatim and spawn nothing (dev/remote).
//   - Otherwise the shell IS the launcher: it spawns the bundled
//     `pinback-server` child on 127.0.0.1:8088, waits for /healthz, then loads
//     it, and kills the child on shutdown. A novice runs one binary, no
//     terminal — the cockpit backend + UI come up underneath it.
#include <gtk/gtk.h>
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

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Pinback");
    gtk_window_set_default_size(GTK_WINDOW(window), 1280, 800);

    GtkWidget *web = webkit_web_view_new();
    const char *url = g_getenv("PINBACK_URL");
    if (!url || !*url) {
        spawn_server();             // self-host the cockpit on loopback
        url = "http://" PB_HOST ":8088";
    }
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(web), url);

    gtk_window_set_child(GTK_WINDOW(window), web);
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    g_autoptr(GtkApplication) app =
        gtk_application_new("dev.pinback.shell", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    stop_server();                  // reap the server when the window closes
    return status;
}
