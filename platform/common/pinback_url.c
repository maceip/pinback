/* getaddrinfo(3) needs POSIX feature macros on Linux/glibc before netdb.h. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE) && !defined(_GNU_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "pinback_url.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <shlobj.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
static int pinback_mkdir(const char *path) { return _mkdir(path); }
static int path_readable(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
#else
#include <pwd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
static int pinback_mkdir(const char *path) { return mkdir(path, 0700); }
static int path_readable(const char *path) { return access(path, R_OK) == 0; }
#endif

const char *pinback_url_default(void) { return "http://127.0.0.1:8088"; }

const char *pinback_url_from_env(void) {
    const char *e = getenv("PINBACK_URL");
    return (e && *e) ? e : NULL;
}

static void trim(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static int ensure_scheme(char *out, size_t cap, const char *in) {
    if (!in || !*in) return -1;
    if (strncmp(in, "http://", 7) == 0 || strncmp(in, "https://", 8) == 0) {
        if (strlen(in) >= cap) return -1;
        strcpy(out, in);
    } else {
        int n = snprintf(out, cap, "http://%s", in);
        if (n < 0 || (size_t)n >= cap) return -1;
    }
    trim(out);
    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == '/') out[--len] = '\0';
    return 0;
}

static int config_dir(char *dir, size_t cap) {
#ifdef _WIN32
    wchar_t w[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, w) != S_OK) return -1;
    char narrow[MAX_PATH];
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, narrow, sizeof narrow, NULL, NULL) <= 0)
        return -1;
    int n = snprintf(dir, cap, "%s\\Pinback", narrow);
#else
    const char *base = getenv("XDG_CONFIG_HOME");
    char home[512];
    if (!base || !*base) {
        const char *h = getenv("HOME");
        if (!h) {
            struct passwd *pw = getpwuid(getuid());
            h = pw ? pw->pw_dir : NULL;
        }
        if (!h) return -1;
        snprintf(home, sizeof home, "%s/.config", h);
        base = home;
    }
    int n = snprintf(dir, cap, "%s/pinback", base);
#endif
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

static int config_path(char *path, size_t cap) {
    char dir[512];
    if (config_dir(dir, sizeof dir) != 0) return -1;
    pinback_mkdir(dir);
#ifdef _WIN32
    int n = snprintf(path, cap, "%s\\server.url", dir);
#else
    int n = snprintf(path, cap, "%s/server.url", dir);
#endif
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

int pinback_url_load_saved(char *out, size_t cap) {
    char path[768];
    if (config_path(path, sizeof path) != 0) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(out, (int)cap, f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    trim(out);
    if (!*out) return 0;
    char norm[PINBACK_URL_MAX];
    if (ensure_scheme(norm, sizeof norm, out) != 0) return 0;
    if (strlen(norm) >= cap) return 0;
    strcpy(out, norm);
    return 1;
}

int pinback_url_save(const char *url) {
    char norm[PINBACK_URL_MAX];
    if (!url || ensure_scheme(norm, sizeof norm, url) != 0) return -1;
    char path[768];
    if (config_path(path, sizeof path) != 0) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(norm, f);
    fputc('\n', f);
    fclose(f);
    return 0;
}

int pinback_url_resolve(char *out, size_t cap, const char *fallback_default) {
    const char *env = pinback_url_from_env();
    if (env) return ensure_scheme(out, cap, env);
    if (pinback_url_load_saved(out, cap)) return 0;
    return ensure_scheme(out, cap,
                         fallback_default ? fallback_default : pinback_url_default());
}

static int parse_http_url(const char *url, char *host, size_t host_cap, int *port) {
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) p += 8;
    else return -1;

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= host_cap) return -1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535) return -1;
    } else {
        size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
        if (hlen >= host_cap) return -1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        *port = (strncmp(url, "https://", 8) == 0) ? 443 : 80;
    }
    return 0;
}

int pinback_health_ok(const char *url) {
    if (!url || !*url) return 0;
    char host[256];
    int port = 8088;
    if (parse_http_url(url, host, sizeof host, &port) != 0) return 0;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
#endif

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portbuf[16];
    snprintf(portbuf, sizeof portbuf, "%d", port);
    if (getaddrinfo(host, portbuf, &hints, &res) != 0) return 0;

    int ok = 0;
    for (struct addrinfo *ai = res; ai && !ok; ai = ai->ai_next) {
        int fd = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
#ifdef _WIN32
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) != 0) {
            closesocket(fd);
            continue;
        }
#else
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            close(fd);
            continue;
        }
#endif
        char req[512];
        int n = snprintf(req, sizeof req,
                         "GET /healthz HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                         host);
        if (n > 0 && n < (int)sizeof req)
#ifdef _WIN32
            send(fd, req, n, 0);
#else
            write(fd, req, (size_t)n);
#endif
        char buf[128] = {0};
#ifdef _WIN32
        int nr = recv(fd, buf, sizeof buf - 1, 0);
#else
        ssize_t nr = read(fd, buf, sizeof buf - 1);
#endif
        ok = (nr > 12 && !memcmp(buf, "HTTP/", 5) && strstr(buf, " 200") != NULL);
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
    }
    freeaddrinfo(res);
#ifdef _WIN32
    WSACleanup();
#endif
    return ok;
}

static int append_path(char *out, size_t cap, const char *base, const char *leaf) {
    size_t n = strlen(base);
    int need_slash = (n > 0 && base[n - 1] != '/' && base[n - 1] != '\\');
    int r = snprintf(out, cap, "%s%s%s", base, need_slash ? "/" : "", leaf);
    return (r < 0 || (size_t)r >= cap) ? -1 : 0;
}

static int exe_directory(char *dir, size_t cap) {
#ifdef _WIN32
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return -1;
    if (WideCharToMultiByte(CP_UTF8, 0, exe, -1, dir, (int)cap, NULL, NULL) <= 0)
        return -1;
    char *slash = strrchr(dir, '\\');
    if (!slash) return -1;
    *slash = '\0';
    return 0;
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)cap;
    if (_NSGetExecutablePath(dir, &sz) != 0) return -1;
    char *slash = strrchr(dir, '/');
    if (!slash) return -1;
    *slash = '\0';
    return 0;
#else
    ssize_t n = readlink("/proc/self/exe", dir, cap - 1);
    if (n <= 0) return -1;
    dir[n] = '\0';
    char *slash = strrchr(dir, '/');
    if (!slash) return -1;
    *slash = '\0';
    return 0;
#endif
}

int pinback_setup_file_uri(char *out, size_t cap) {
    const char *env = getenv("PINBACK_SETUP_HTML");
    if (env && *env) {
        snprintf(out, cap, "file://%s", env);
        return 0;
    }

    char dir[1024];
    char path[1200];
    if (exe_directory(dir, sizeof dir) == 0) {
        if (append_path(path, sizeof path, dir, "setup.html") == 0 &&
            path_readable(path)) {
            snprintf(out, cap, "file://%s", path);
            return 0;
        }
#if defined(__APPLE__)
        if (append_path(path, sizeof path, dir, "../Resources/setup.html") == 0 &&
            path_readable(path)) {
            snprintf(out, cap, "file://%s", path);
            return 0;
        }
#endif
    }
    if (append_path(path, sizeof path, "platform/common", "setup.html") == 0 &&
        path_readable(path)) {
        snprintf(out, cap, "file://%s", path);
        return 0;
    }
    return -1;
}
