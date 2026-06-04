#include "../src/http.h"
#include "../src/util.h"

int run_http_tests(void);

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fails = 0;
#define EXPECT(c, m) do { \
    if (c) printf("ok  - %s\n", m); \
    else { printf("not ok - %s (%s:%d)\n", m, __FILE__, __LINE__); fails++; } \
} while (0)

static void send_str(int fd, const char *s) {
    size_t n = strlen(s);
    while (n > 0) {
        ssize_t w = send(fd, s, n, 0);
        if (w <= 0) break;
        s += w; n -= (size_t)w;
    }
}

static void test_parse_get(void) {
    int sp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    const char *raw =
        "GET /api/events?after=42&generation=3 HTTP/1.1\r\n"
        "Host: x\r\n"
        "Last-Event-ID: 99\r\n"
        "X-Request-Id: abc\r\n"
        "\r\n";
    send_str(sp[0], raw);
    shutdown(sp[0], SHUT_WR);
    pin_request r = {0};
    EXPECT(pin_http_read_request(sp[1], &r), "parse GET");
    EXPECT(strcmp(r.method, "GET") == 0, "method GET");
    EXPECT(strcmp(r.path, "/api/events") == 0, "path stripped of query");
    const char *v; size_t vl;
    EXPECT(pin_http_query(&r, "after", &v, &vl) && vl == 2 && memcmp(v, "42", 2) == 0,
           "query after");
    EXPECT(pin_http_query(&r, "generation", &v, &vl) && vl == 1 && v[0] == '3',
           "query generation");
    EXPECT(pin_http_header(&r, "last-event-id", &v, &vl) && vl == 2 && memcmp(v, "99", 2) == 0,
           "header case-insensitive");
    pin_request_free(&r);
    close(sp[0]); close(sp[1]);
}

static void test_smuggling_rejected(void) {
    int sp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    /* Both Transfer-Encoding and Content-Length: must reject. */
    const char *raw =
        "POST /api/input HTTP/1.1\r\n"
        "Host: x\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    send_str(sp[0], raw);
    shutdown(sp[0], SHUT_WR);
    pin_request r = {0};
    bool ok = pin_http_read_request(sp[1], &r);
    EXPECT(!ok, "rejects request with both TE and CL");
    pin_request_free(&r);
    close(sp[0]); close(sp[1]);
}

static void test_post_body(void) {
    int sp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    const char *raw =
        "POST /api/input HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 13\r\n"
        "Content-Type: application/json\r\n"
        "\r\n"
        "{\"text\":\"hi\"}";
    send_str(sp[0], raw);
    shutdown(sp[0], SHUT_WR);
    pin_request r = {0};
    EXPECT(pin_http_read_request(sp[1], &r), "parse POST");
    EXPECT(r.body_len == 13, "body len");
    EXPECT(memcmp(r.body, "{\"text\":\"hi\"}", 13) == 0, "body bytes");
    pin_request_free(&r);
    close(sp[0]); close(sp[1]);
}

int run_http_tests(void) {
    test_parse_get();
    test_smuggling_rejected();
    test_post_body();
    return fails;
}
