#include "http.h"

#include "log.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

/* ====================================================================
 * Network sugar — sane timeouts, no-SIGPIPE, NODELAY.
 * ==================================================================== */

#ifdef __APPLE__
#define PIN_MSG_NOSIGNAL 0
#else
#define PIN_MSG_NOSIGNAL MSG_NOSIGNAL
#endif

static void apply_socket_options(int fd)
{
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
    struct timeval tv = {.tv_sec = PIN_HTTP_IO_TIMEOUT_SEC, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static ssize_t send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = send(fd, p, left, PIN_MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        p += n;
        left -= (size_t)n;
    }
    return (ssize_t)len;
}

static ssize_t writev_all(int fd, struct iovec *iov, int n)
{
    /* Loop until all iov entries are sent. We rebuild the iovec in
     * place when partial writes occur. */
    while (n > 0) {
        ssize_t w = writev(fd, iov, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        while (n > 0 && (size_t)w >= iov[0].iov_len) {
            w -= (ssize_t)iov[0].iov_len;
            iov++;
            n--;
        }
        if (n > 0) {
            iov[0].iov_base = (char *)iov[0].iov_base + w;
            iov[0].iov_len -= (size_t)w;
        }
    }
    return 0;
}

static ssize_t recv_some(int fd, void *buf, size_t cap)
{
    while (1) {
        ssize_t n = recv(fd, buf, cap, 0);
        if (n < 0 && errno == EINTR)
            continue;
        return n;
    }
}

static ssize_t recv_n(int fd, void *buf, size_t n)
{
    char *p = buf;
    size_t left = n;
    while (left > 0) {
        ssize_t r = recv_some(fd, p, left);
        if (r <= 0)
            return r < 0 ? r : 0;
        p += r;
        left -= (size_t)r;
    }
    return (ssize_t)n;
}

/* ====================================================================
 * Request parser
 * ==================================================================== */

void pin_request_free(pin_request *r)
{
    if (!r)
        return;
    free(r->method);
    free(r->path);
    free(r->query);
    free(r->headers_blob);
    free(r->body);
    memset(r, 0, sizeof(*r));
}

/* Read until "\r\n\r\n" or PIN_HTTP_MAX_HEADERS_BYTES. Returns the number
 * of bytes read into buf (which is at least cap+1). On success, sets
 * *header_end to the offset after the terminating CRLF CRLF. */
static bool read_headers(int fd, pin_buf *buf, size_t *header_end)
{
    char tmp[2048];
    size_t total = 0;
    while (total < PIN_HTTP_MAX_HEADERS_BYTES) {
        ssize_t n = recv_some(fd, tmp, sizeof(tmp));
        if (n <= 0)
            return false;
        pin_buf_append(buf, tmp, (size_t)n);
        total += (size_t)n;
        const char *m = memmem(buf->ptr, buf->len, "\r\n\r\n", 4);
        if (m) {
            *header_end = (size_t)(m - buf->ptr) + 4;
            return true;
        }
    }
    return false;
}

static char *dup_token(const char *s, size_t n)
{
    /* Reject CR/LF/NUL/control chars in tokens. */
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F)
            return NULL;
    }
    return pin_xstrndup(s, n);
}

bool pin_http_read_request(int fd, pin_request *r)
{
    memset(r, 0, sizeof(*r));
    apply_socket_options(fd);
    pin_random_hex(r->request_id, sizeof(r->request_id), 16);

    pin_buf raw;
    pin_buf_init(&raw);
    size_t header_end = 0;
    if (!read_headers(fd, &raw, &header_end)) {
        pin_buf_free(&raw);
        return false;
    }

    /* Parse request line: METHOD SP PATH[?QUERY] SP HTTP/1.1 CRLF */
    char *line_end = memmem(raw.ptr, header_end - 4, "\r\n", 2);
    if (!line_end) {
        pin_buf_free(&raw);
        return false;
    }
    size_t line_len = (size_t)(line_end - raw.ptr);
    if (line_len < 14 || line_len > 4096) {
        pin_buf_free(&raw);
        return false;
    }

    const char *p = raw.ptr;
    const char *sp1 = memchr(p, ' ', line_len);
    if (!sp1) {
        pin_buf_free(&raw);
        return false;
    }
    const char *sp2 = memchr(sp1 + 1, ' ', line_len - (size_t)(sp1 + 1 - p));
    if (!sp2) {
        pin_buf_free(&raw);
        return false;
    }

    r->method = dup_token(p, (size_t)(sp1 - p));
    if (!r->method) {
        pin_buf_free(&raw);
        return false;
    }

    /* Path may contain '?' */
    const char *path_start = sp1 + 1;
    size_t path_len = (size_t)(sp2 - path_start);
    if (path_len == 0 || path_len > 4096) {
        pin_buf_free(&raw);
        pin_request_free(r);
        return false;
    }
    const char *qmark = memchr(path_start, '?', path_len);
    if (qmark) {
        r->path = dup_token(path_start, (size_t)(qmark - path_start));
        size_t qlen = path_len - (size_t)(qmark - path_start) - 1;
        r->query = dup_token(qmark + 1, qlen);
        if (!r->path || !r->query) {
            pin_buf_free(&raw);
            pin_request_free(r);
            return false;
        }
    } else {
        r->path = dup_token(path_start, path_len);
        if (!r->path) {
            pin_buf_free(&raw);
            pin_request_free(r);
            return false;
        }
    }

    /* Version: only HTTP/1.1 or HTTP/1.0 allowed. */
    const char *ver = sp2 + 1;
    size_t ver_len = line_len - (size_t)(sp2 - p) - 1;
    if (ver_len != 8 || (memcmp(ver, "HTTP/1.1", 8) != 0 && memcmp(ver, "HTTP/1.0", 8) != 0)) {
        pin_buf_free(&raw);
        pin_request_free(r);
        return false;
    }

    /* Headers blob is the bytes between the request line CRLF and the
     * terminating empty line, joined as-is. */
    size_t hdr_start = (size_t)(line_end - raw.ptr) + 2;
    size_t hdr_len = header_end - 2 - hdr_start;
    r->headers_blob = pin_xmalloc(hdr_len + 1);
    memcpy(r->headers_blob, raw.ptr + hdr_start, hdr_len);
    r->headers_blob[hdr_len] = '\0';
    r->headers_len = hdr_len;

    /* Reject Transfer-Encoding entirely (smuggling defense). */
    const char *te = NULL;
    size_t te_len = 0;
    if (pin_http_header(r, "Transfer-Encoding", &te, &te_len)) {
        pin_buf_free(&raw);
        pin_request_free(r);
        return false;
    }
    /* Reject duplicate Content-Length (smuggling defense). */
    {
        size_t count = 0;
        const char *cur = r->headers_blob;
        while ((cur = memmem(cur, r->headers_len - (size_t)(cur - r->headers_blob),
                             "Content-Length:", 15)) != NULL) {
            if (cur == r->headers_blob || cur[-1] == '\n')
                count++;
            cur += 15;
        }
        if (count > 1) {
            pin_buf_free(&raw);
            pin_request_free(r);
            return false;
        }
    }

    /* Body. */
    size_t content_length = 0;
    const char *cl = NULL;
    size_t cl_len = 0;
    if (pin_http_header(r, "Content-Length", &cl, &cl_len)) {
        if (cl_len == 0 || cl_len > 16) {
            pin_buf_free(&raw);
            pin_request_free(r);
            return false;
        }
        char tmp[20];
        memcpy(tmp, cl, cl_len);
        tmp[cl_len] = '\0';
        char *end = NULL;
        unsigned long long v = strtoull(tmp, &end, 10);
        if (end == tmp || *end != '\0') {
            pin_buf_free(&raw);
            pin_request_free(r);
            return false;
        }
        if (v > PIN_HTTP_MAX_BODY_BYTES) {
            pin_buf_free(&raw);
            pin_request_free(r);
            return false;
        }
        content_length = (size_t)v;
    }

    if (content_length > 0) {
        r->body = pin_xmalloc(content_length + 1);
        size_t already = raw.len - header_end;
        if (already > content_length)
            already = content_length;
        memcpy(r->body, raw.ptr + header_end, already);
        if (already < content_length) {
            ssize_t got = recv_n(fd, r->body + already, content_length - already);
            if (got <= 0) {
                pin_buf_free(&raw);
                pin_request_free(r);
                return false;
            }
        }
        r->body[content_length] = '\0';
        r->body_len = content_length;
    }

    pin_buf_free(&raw);
    return true;
}

bool pin_http_header(const pin_request *r, const char *name, const char **out, size_t *out_len)
{
    if (!r || !r->headers_blob)
        return false;
    size_t name_len = strlen(name);
    const char *p = r->headers_blob;
    const char *end = p + r->headers_len;
    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        size_t llen = line_end ? (size_t)(line_end - p) : (size_t)(end - p);
        if (llen > 0 && p[llen - 1] == '\r')
            llen--;
        if (llen > name_len + 1 && strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
            const char *v = p + name_len + 1;
            size_t vlen = llen - name_len - 1;
            while (vlen && (*v == ' ' || *v == '\t')) {
                v++;
                vlen--;
            }
            *out = v;
            *out_len = vlen;
            return true;
        }
        if (!line_end)
            break;
        p = line_end + 1;
    }
    return false;
}

bool pin_http_query(const pin_request *r, const char *key, const char **out, size_t *out_len)
{
    if (!r || !r->query)
        return false;
    size_t klen = strlen(key);
    const char *p = r->query;
    while (*p) {
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        if (!amp)
            amp = p + strlen(p);
        if (eq && eq < amp && (size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
            *out = eq + 1;
            *out_len = (size_t)(amp - eq - 1);
            return true;
        }
        if (*amp == '\0')
            break;
        p = amp + 1;
    }
    return false;
}

/* ====================================================================
 * Response helpers
 * ==================================================================== */

static const char *status_text(int s)
{
    switch (s) {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 202:
        return "Accepted";
    case 204:
        return "No Content";
    case 301:
        return "Moved Permanently";
    case 304:
        return "Not Modified";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 413:
        return "Payload Too Large";
    case 415:
        return "Unsupported Media Type";
    case 429:
        return "Too Many Requests";
    case 500:
        return "Internal Server Error";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    default:
        return "OK";
    }
}

static void emit_security_headers(pin_buf *b)
{
    /* CSP per ARCHITECTURE_REDO.md §"Untrusted Content And Safety". The
     * UI ships split files (index.html + app.css + app.js), so we never
     * need 'unsafe-inline' for scripts. */
    /* 'wasm-unsafe-eval' is required so the bundled Shiki highlighter can
     * compile its onig.wasm (vscode-oniguruma, the same regex engine VS
     * Code and Jules ship). It permits WASM module compilation only; it
     * does NOT loosen JS eval/inline-script restrictions. */
    pin_buf_puts(b, "Content-Security-Policy: "
                    "default-src 'self'; "
                    "script-src 'self' 'wasm-unsafe-eval'; "
                    "style-src 'self' 'unsafe-inline'; "
                    "img-src 'self' data:; "
                    "connect-src 'self'; "
                    "frame-ancestors 'none'; "
                    "base-uri 'none'; "
                    "form-action 'self'\r\n");
    pin_buf_puts(b, "X-Content-Type-Options: nosniff\r\n");
    pin_buf_puts(b, "Referrer-Policy: no-referrer\r\n");
    pin_buf_puts(b, "Permissions-Policy: geolocation=(), microphone=(), camera=()\r\n");
}

bool pin_http_respond(int fd, const pin_request *r, int status, const char *content_type,
                      const char *body, size_t body_len)
{
    pin_buf hdr;
    pin_buf_init(&hdr);
    pin_buf_printf(&hdr, "HTTP/1.1 %d %s\r\n", status, status_text(status));
    pin_buf_puts(&hdr, "Server: pinback\r\n");
    pin_buf_puts(&hdr, "Connection: close\r\n");
    if (r)
        pin_buf_printf(&hdr, "X-Request-Id: %s\r\n", r->request_id);
    if (content_type)
        pin_buf_printf(&hdr, "Content-Type: %s\r\n", content_type);
    pin_buf_printf(&hdr, "Content-Length: %zu\r\n", body_len);
    emit_security_headers(&hdr);
    pin_buf_puts(&hdr, "\r\n");
    bool ok = (send_all(fd, hdr.ptr, hdr.len) >= 0);
    if (ok && body_len)
        ok = (send_all(fd, body, body_len) >= 0);
    pin_buf_free(&hdr);
    return ok;
}

bool pin_http_respond_text(int fd, const pin_request *r, int status, const char *text)
{
    return pin_http_respond(fd, r, status, "text/plain; charset=utf-8", text,
                            text ? strlen(text) : 0);
}

bool pin_http_respond_json(int fd, const pin_request *r, int status, const pin_buf *body)
{
    return pin_http_respond(fd, r, status, "application/json; charset=utf-8",
                            body && body->ptr ? body->ptr : "{}", body ? body->len : 2);
}

bool pin_http_respond_error(int fd, const pin_request *r, int status, const char *code,
                            const char *message)
{
    pin_buf b;
    pin_buf_init(&b);
    pin_buf_putc(&b, '{');
    pin_buf_puts(&b, "\"code\":");
    pin_json_str(&b, code ? code : "error");
    pin_buf_puts(&b, ",\"message\":");
    pin_buf clean;
    pin_buf_init(&clean);
    pin_text_sanitize(&clean, message ? message : "", message ? strlen(message) : 0, 1024);
    pin_json_str(&b, clean.ptr ? clean.ptr : "");
    pin_buf_free(&clean);
    if (r) {
        pin_buf_puts(&b, ",\"request_id\":");
        pin_json_str(&b, r->request_id);
    }
    pin_buf_putc(&b, '}');
    bool ok = pin_http_respond_json(fd, r, status, &b);
    pin_buf_free(&b);
    return ok;
}

/* ====================================================================
 * SSE
 * ==================================================================== */

bool pin_http_begin_sse(int fd, const pin_request *r)
{
    pin_buf hdr;
    pin_buf_init(&hdr);
    pin_buf_puts(&hdr, "HTTP/1.1 200 OK\r\n");
    pin_buf_puts(&hdr, "Server: pinback\r\n");
    pin_buf_puts(&hdr, "Content-Type: text/event-stream; charset=utf-8\r\n");
    pin_buf_puts(&hdr, "Cache-Control: no-cache, no-store, must-revalidate\r\n");
    pin_buf_puts(&hdr, "X-Accel-Buffering: no\r\n");
    pin_buf_puts(&hdr, "Connection: close\r\n");
    if (r)
        pin_buf_printf(&hdr, "X-Request-Id: %s\r\n", r->request_id);
    emit_security_headers(&hdr);
    pin_buf_puts(&hdr, "\r\n");
    bool ok = (send_all(fd, hdr.ptr, hdr.len) >= 0);
    pin_buf_free(&hdr);
    return ok;
}

bool pin_http_sse_emit(int fd, long long id, const char *event, const char *data, size_t data_len)
{
    /* Build the prefix in one stack buffer, then writev with the data. */
    char prefix[128];
    int n = 0;
    if (id >= 0) {
        n = snprintf(prefix, sizeof(prefix), "id: %lld\n", id);
        if (n < 0)
            return false;
    }
    if (event && *event) {
        int m = snprintf(prefix + n, sizeof(prefix) - (size_t)n, "event: %s\n", event);
        if (m < 0)
            return false;
        n += m;
    }
    /* "data: " prefix then the bytes (which must already not contain a
     * line ending other than \n, and even \n inside breaks SSE; our
     * payloads are JSON which is safe). */
    int m = snprintf(prefix + n, sizeof(prefix) - (size_t)n, "data: ");
    if (m < 0)
        return false;
    n += m;

    struct iovec iov[3];
    iov[0].iov_base = prefix;
    iov[0].iov_len = (size_t)n;
    iov[1].iov_base = (void *)data;
    iov[1].iov_len = data_len;
    iov[2].iov_base = (void *)"\n\n";
    iov[2].iov_len = 2;
    return writev_all(fd, iov, 3) == 0;
}

bool pin_http_sse_keepalive(int fd)
{
    return send_all(fd, ":\n\n", 3) >= 0;
}

/* ====================================================================
 * WebSocket — server side, mirrors ds4_web.c framing
 * ==================================================================== */

static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

bool pin_ws_upgrade(int fd, const pin_request *r)
{
    /* Validate Upgrade: websocket and Connection: upgrade. */
    const char *v = NULL;
    size_t vlen = 0;
    if (!pin_http_header(r, "Upgrade", &v, &vlen))
        return false;
    if (vlen < 9 || strncasecmp(v, "websocket", 9) != 0)
        return false;
    if (!pin_http_header(r, "Connection", &v, &vlen))
        return false;
    /* "Connection" can be a comma list: search for "upgrade" token. */
    {
        char tmp[128];
        if (vlen >= sizeof(tmp))
            return false;
        memcpy(tmp, v, vlen);
        tmp[vlen] = '\0';
        bool found = false;
        for (char *t = strtok(tmp, ",; "); t; t = strtok(NULL, ",; ")) {
            if (!strcasecmp(t, "upgrade")) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    if (!pin_http_header(r, "Sec-WebSocket-Version", &v, &vlen))
        return false;
    if (vlen != 2 || memcmp(v, "13", 2) != 0)
        return false;

    const char *key = NULL;
    size_t klen = 0;
    if (!pin_http_header(r, "Sec-WebSocket-Key", &key, &klen))
        return false;
    if (klen < 16 || klen > 64)
        return false;

    /* accept = base64(sha1(key || GUID)) */
    pin_buf b;
    pin_buf_init(&b);
    pin_buf_append(&b, key, klen);
    pin_buf_puts(&b, WS_GUID);
    uint8_t digest[20];
    pin_sha1_bytes(b.ptr, b.len, digest);
    pin_buf_free(&b);
    char accept[29];
    pin_base64_20(digest, accept);

    pin_buf out;
    pin_buf_init(&out);
    pin_buf_puts(&out, "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: ");
    pin_buf_puts(&out, accept);
    pin_buf_puts(&out, "\r\nServer: pinback\r\n");
    if (r)
        pin_buf_printf(&out, "X-Request-Id: %s\r\n", r->request_id);
    pin_buf_puts(&out, "\r\n");
    bool ok = (send_all(fd, out.ptr, out.len) >= 0);
    pin_buf_free(&out);
    return ok;
}

static bool ws_send_frame(int fd, int opcode, const void *payload, size_t len)
{
    uint8_t hdr[10];
    size_t hdr_len = 0;
    hdr[0] = (uint8_t)(0x80 | (opcode & 0x0F)); /* FIN=1 */
    if (len < 126) {
        hdr[1] = (uint8_t)len;
        hdr_len = 2;
    } else if (len < 65536) {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)len;
        hdr_len = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++)
            hdr[2 + i] = (uint8_t)(len >> (56 - i * 8));
        hdr_len = 10;
    }
    struct iovec iov[2] = {
        {.iov_base = hdr, .iov_len = hdr_len},
        {.iov_base = (void *)payload, .iov_len = len},
    };
    return writev_all(fd, iov, len ? 2 : 1) == 0;
}

bool pin_ws_send_text(int fd, const char *data, size_t len)
{
    return ws_send_frame(fd, 0x1, data, len);
}

bool pin_ws_send_close(int fd, uint16_t code)
{
    uint8_t payload[2];
    payload[0] = (uint8_t)(code >> 8);
    payload[1] = (uint8_t)code;
    return ws_send_frame(fd, 0x8, payload, 2);
}

bool pin_ws_send_pong(int fd, const uint8_t *payload, size_t len)
{
    if (len > 125)
        len = 125;
    return ws_send_frame(fd, 0xA, payload, len);
}

#define PIN_WS_MAX_PAYLOAD (1u << 20) /* 1 MiB */
#define PIN_WS_MAX_CTRL_PAYLOAD 125u

bool pin_ws_read_frame(int fd, int *out_opcode, uint8_t **out_payload, size_t *out_len)
{
    *out_payload = NULL;
    *out_len = 0;
    *out_opcode = -1;

    uint8_t h[2];
    if (recv_n(fd, h, 2) != 2)
        return false;

    bool fin = (h[0] & 0x80) != 0;
    int rsv = h[0] & 0x70;
    int op = h[0] & 0x0F;
    bool mask = (h[1] & 0x80) != 0;
    uint64_t plen = h[1] & 0x7F;

    if (rsv != 0)
        return false; /* reserved bits must be 0 */
    if (!fin)
        return false; /* v1: no fragmentation */
    if (!mask)
        return false; /* client->server MUST mask */
    if (op > 0xA || (op > 0x2 && op < 0x8))
        return false; /* reserved opcodes */

    if (plen == 126) {
        uint8_t e[2];
        if (recv_n(fd, e, 2) != 2)
            return false;
        plen = ((uint64_t)e[0] << 8) | e[1];
    } else if (plen == 127) {
        uint8_t e[8];
        if (recv_n(fd, e, 8) != 8)
            return false;
        plen = 0;
        for (int i = 0; i < 8; i++)
            plen = (plen << 8) | e[i];
    }

    bool is_ctrl = (op & 0x8) != 0;
    if (is_ctrl && plen > PIN_WS_MAX_CTRL_PAYLOAD)
        return false;
    if (plen > PIN_WS_MAX_PAYLOAD)
        return false;

    uint8_t mkey[4];
    if (recv_n(fd, mkey, 4) != 4)
        return false;

    uint8_t *payload = NULL;
    if (plen > 0) {
        payload = pin_xmalloc((size_t)plen + 1);
        if (recv_n(fd, payload, (size_t)plen) != (ssize_t)plen) {
            free(payload);
            return false;
        }
        for (uint64_t i = 0; i < plen; i++)
            payload[i] ^= mkey[i & 3];
        payload[plen] = '\0';
    }

    *out_opcode = op;
    *out_payload = payload;
    *out_len = (size_t)plen;
    return true;
}
