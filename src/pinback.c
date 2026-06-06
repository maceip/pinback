/* pinback-server: single-binary GUI cockpit for ds4-agent.
 *
 * v0 model:
 *   - One ds4-agent child process at any moment.
 *   - Multiple workspaces persisted under ~/.pinback; pinback owns the
 *     catalog and per-workspace event logs.
 *   - Switching the active workspace runs save/kill/respawn against
 *     ds4-agent's --chdir + /switch <sha> contract, so each workspace
 *     resumes where it left off (subject to RAM + the model staying
 *     mmap'd in the OS page cache between spawns).
 *
 * Power-user flags:
 *   --bind ADDR        listen address (default 127.0.0.1:8088)
 *   --agent-bin PATH   ds4-agent binary (default ds4-agent on PATH)
 *   --model PATH       --model arg passed through to ds4-agent
 *   --workspace PATH   on first run, create+activate a workspace at PATH
 *   --root DIR         persistent state root (default ~/.pinback)
 *   --ring-cap N       per-workspace ring size (default 4096)
 *   --web-dir DIR      serve UI from DIR
 *   --dev              read UI from ui/app/ on every request
 *   --quiet            warnings + errors only
 *   --help, -h
 */

#include "agent.h"
#include "event_log.h"
#include "handlers.h"
#include "http.h"
#include "log.h"
#include "util.h"
#include "workspace.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(FILE *fp)
{
    fputs("pinback-server: single-binary GUI for ds4-agent\n"
          "\n"
          "Usage: pinback-server [flags]\n"
          "\n"
          "  --bind ADDR        listen address (default 127.0.0.1:8088)\n"
          "  --agent-bin PATH   ds4-agent binary (default 'ds4-agent' on PATH)\n"
          "  --model PATH       --model passed through to ds4-agent\n"
          "  --workspace PATH   create+activate a workspace at PATH on launch\n"
          "  --root DIR         state root (default ~/.pinback)\n"
          "  --ring-cap N       per-workspace ring size (default 4096)\n"
          "  --kvcache-dir DIR  ds4 KV cache dir (default $HOME/.ds4/kvcache)\n"
          "  --save-timeout-ms N  /save wait on workspace switch (default 15000)\n"
          "  --web-dir DIR      serve UI from disk\n"
          "  --dev              dev mode: read ui/app/ from disk\n"
          "  --quiet            only warnings and errors\n"
          "  --no-kv-resume     Disable exact KV session restore (/save+/switch).\n"
          "                     Uses transcript re-prefill on workspace switch instead.\n"
          "                     Default: KV resume ON (TUI over pipes + --trace prose).\n"
          "  --help, -h         show this message\n",
          fp);
}

static bool parse_bind(const char *s, char *host, size_t hostcap, int *port)
{
    if (s[0] == '[') {
        const char *rb = strchr(s, ']');
        if (!rb || rb[1] != ':')
            return false;
        size_t hl = (size_t)(rb - s - 1);
        if (hl >= hostcap)
            return false;
        memcpy(host, s + 1, hl);
        host[hl] = '\0';
        *port = atoi(rb + 2);
    } else {
        const char *colon = strrchr(s, ':');
        if (!colon || colon == s)
            return false;
        size_t hl = (size_t)(colon - s);
        if (hl >= hostcap)
            return false;
        memcpy(host, s, hl);
        host[hl] = '\0';
        *port = atoi(colon + 1);
    }
    return *port > 0 && *port < 65536;
}

static int listen_on(const char *host, int port)
{
    char service[16];
    snprintf(service, sizeof(service), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    const char *node = NULL;
    if (host && host[0] && strcmp(host, "0.0.0.0") != 0) {
        hints.ai_flags &= ~AI_PASSIVE;
        node = host;
    }

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(node, service, &hints, &res);
    if (gai != 0) {
        PIN_LOG_ERRF("boot.bad_host", "host=%s gai=%s", host ? host : "(any)", gai_strerror(gai));
        return -1;
    }

    int listen_fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
#ifdef IPV6_V6ONLY
        if (ai->ai_family == AF_INET6) {
            int v6only = 0;
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
        }
#endif
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            close(fd);
            continue;
        }
        if (listen(fd, 64) != 0) {
            close(fd);
            continue;
        }
        listen_fd = fd;
        break;
    }
    freeaddrinfo(res);

    if (listen_fd < 0) {
        PIN_LOG_ERRF("boot.bind_fail", "host=%s port=%d errno=%d", host ? host : "0.0.0.0", port,
                     errno);
        return -1;
    }
    return listen_fd;
}

static char *default_root(void)
{
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw)
            home = pw->pw_dir;
    }
    if (!home)
        return pin_xstrdup("./.pinback");
    char path[2048];
    snprintf(path, sizeof(path), "%s/.pinback", home);
    return pin_xstrdup(path);
}

typedef struct {
    pin_app *app;
    int fd;
} conn_args;

static void *connection_thread(void *arg)
{
    conn_args *ca = arg;
    pin_handle_connection(ca->app, ca->fd);
    free(ca);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *bind_addr = "127.0.0.1:8088";
    const char *agent_bin = "ds4-agent";
    const char *model_path = NULL;
    const char *workspace_path = NULL;
    char *root_dir = NULL;
    size_t ring_cap = 4096;
    const char *web_dir = NULL;
    const char *kvcache_dir = NULL;
    int save_timeout_ms = 0; /* 0 => use default from kv_resume */
    bool dev_mode = false;
    bool quiet = false;
    bool kv_resume = true;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "--bind") && i + 1 < argc)
            bind_addr = argv[++i];
        else if (!strcmp(a, "--agent-bin") && i + 1 < argc)
            agent_bin = argv[++i];
        else if (!strcmp(a, "--model") && i + 1 < argc)
            model_path = argv[++i];
        else if (!strcmp(a, "--workspace") && i + 1 < argc)
            workspace_path = argv[++i];
        else if (!strcmp(a, "--root") && i + 1 < argc)
            root_dir = pin_xstrdup(argv[++i]);
        else if (!strcmp(a, "--ring-cap") && i + 1 < argc)
            ring_cap = (size_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--kvcache-dir") && i + 1 < argc)
            kvcache_dir = argv[++i];
        else if (!strcmp(a, "--save-timeout-ms") && i + 1 < argc)
            save_timeout_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--web-dir") && i + 1 < argc)
            web_dir = argv[++i];
        else if (!strcmp(a, "--dev"))
            dev_mode = true;
        else if (!strcmp(a, "--quiet"))
            quiet = true;
        else if (!strcmp(a, "--no-kv-resume"))
            kv_resume = false;
        else if (!strcmp(a, "--kv-resume"))
            kv_resume = true; /* legacy alias; KV is default */
        else {
            fprintf(stderr, "pinback-server: unknown flag '%s'\n", a);
            usage(stderr);
            return 2;
        }
    }
    if (!root_dir)
        root_dir = default_root();
    if (dev_mode && !web_dir)
        web_dir = "ui/app";

    pin_log_init("pinback-server", quiet ? PIN_LOG_WARN : PIN_LOG_INFO);

    char host[128];
    int port = 0;
    if (!parse_bind(bind_addr, host, sizeof(host), &port)) {
        fprintf(stderr, "pinback-server: bad --bind '%s'\n", bind_addr);
        free(root_dir);
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    pin_workspace_store *store = pin_workspace_store_open(root_dir, ring_cap);
    if (!store) {
        PIN_LOG_ERRF("boot.workspace_store_fail", "root=%s", root_dir);
        free(root_dir);
        return 1;
    }

    pin_agent_config acfg = {
        .agent_bin = agent_bin,
        .model_path = model_path,
        .kvcache_dir = kvcache_dir,
        .spawn_ready_ms = 30000,
        .save_timeout_ms = save_timeout_ms > 0 ? save_timeout_ms : (kv_resume ? 15000 : 0),
        .term_timeout_ms = 5000,
        .kv_resume = kv_resume,
    };
    pin_agent *agent = pin_agent_new(&acfg, store);
    if (!agent) {
        pin_workspace_store_close(store);
        free(root_dir);
        return 1;
    }

    /* If --workspace given, ensure-create + activate. Otherwise, if a
     * prior active workspace is on disk, restore it. */
    char *active_id = NULL;
    if (workspace_path) {
        /* If a workspace already exists at that path, find its id; else
         * create. */
        size_t n = 0;
        pin_workspace_meta *list = pin_workspace_store_list(store, &n);
        char *found = NULL;
        for (size_t i = 0; i < n; i++) {
            if (list[i].path && !strcmp(list[i].path, workspace_path)) {
                found = pin_xstrdup(list[i].id);
                break;
            }
        }
        pin_workspace_meta_array_free(list, n);
        if (found) {
            active_id = found;
        } else {
            char *err = NULL;
            if (!pin_workspace_store_create(store, workspace_path, NULL, &active_id, &err)) {
                fprintf(stderr, "pinback-server: --workspace failed: %s\n", err ? err : "unknown");
                free(err);
                pin_agent_free(agent);
                pin_workspace_store_close(store);
                free(root_dir);
                return 1;
            }
        }
    }
    if (!active_id) {
        active_id = pin_workspace_store_get_active(store);
    }
    if (active_id) {
        char *err = NULL;
        if (!pin_agent_activate(agent, active_id, &err)) {
            PIN_LOG_WARNF("boot.activate_fail", "id=%s err=%s", active_id, err ? err : "?");
            free(err);
        }
    }

    int sfd = listen_on(host, port);
    if (sfd < 0) {
        free(active_id);
        pin_agent_free(agent);
        pin_workspace_store_close(store);
        free(root_dir);
        return 1;
    }

    pin_app app = {
        .ws = store,
        .agent = agent,
        .web_root = web_dir,
        .bind_str = bind_addr,
        .started_ms = (long long)pin_monotonic_ms(),
        .dev_mode = dev_mode,
    };

    PIN_LOG_INFOF(PIN_EV_BOOT_LISTEN, "bind=%s agent_bin=%s root=%s active_id=%s", bind_addr,
                  agent_bin, root_dir, active_id ? active_id : "(none)");
    fprintf(stderr,
            "\n"
            "  pinback-server up at http://%s\n"
            "  agent: %s%s%s\n"
            "  state root: %s\n"
            "  open the URL above in your browser; ^C to stop\n"
            "\n",
            bind_addr, agent_bin, model_path ? "  --model " : "", model_path ? model_path : "",
            root_dir);
    free(active_id);

    while (!g_stop) {
        struct pollfd pfd = {.fd = sfd, .events = POLLIN};
        int pr = poll(&pfd, 1, 1000);
        if (pr <= 0) {
            if (pr < 0 && errno != EINTR) {
                PIN_LOG_WARNF("poll_err", "errno=%d", errno);
            }
            continue;
        }
        struct sockaddr_storage cli;
        socklen_t cl = sizeof(cli);
        int cfd = accept(sfd, (struct sockaddr *)&cli, &cl);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            PIN_LOG_WARNF("accept_err", "errno=%d", errno);
            continue;
        }
        int yes = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
#ifdef SO_NOSIGPIPE
        setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
        struct timeval tv = {.tv_sec = 30};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        conn_args *ca = pin_xcalloc(1, sizeof(*ca));
        ca->app = &app;
        ca->fd = cfd;

        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &attr, connection_thread, ca) != 0) {
            close(cfd);
            free(ca);
            PIN_LOG_WARNF("thread_spawn_fail", "errno=%d", errno);
        }
        pthread_attr_destroy(&attr);
    }

    PIN_LOG_INFOF(PIN_EV_BOOT_SHUTDOWN, "shutdown_initiated");
    close(sfd);
    pin_agent_free(agent);
    pin_workspace_store_close(store);
    free(root_dir);
    PIN_LOG_INFOF(PIN_EV_BOOT_SHUTDOWN, "shutdown_complete");
    return 0;
}
