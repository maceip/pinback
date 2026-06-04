#include "log.h"

#include "util.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

/* JSON-Lines logger. One write(2) per record so concurrent goroutines
 * cannot interleave half a line.
 *
 * Style: copy what ds4_server.c does for log buffers, but emit JSON
 * instead of a colored TTY line. */

struct pin_log_rec {
    pin_buf buf;
    pin_log_level lvl;
};

static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
static pin_log_level   g_log_min = PIN_LOG_INFO;
static char            g_log_service[32] = "pinback";

void pin_log_init(const char *service, pin_log_level min_level) {
    if (service && *service) {
        snprintf(g_log_service, sizeof(g_log_service), "%s", service);
    }
    g_log_min = min_level;
}

void pin_log_set_min_level(pin_log_level lvl) { g_log_min = lvl; }

static const char *level_name(pin_log_level lvl) {
    switch (lvl) {
    case PIN_LOG_DEBUG: return "debug";
    case PIN_LOG_INFO:  return "info";
    case PIN_LOG_WARN:  return "warn";
    case PIN_LOG_ERROR: return "error";
    }
    return "info";
}

pin_log_rec *pin_log_begin(pin_log_level lvl, const char *event) {
    if ((int)lvl < (int)g_log_min) return NULL;
    pin_log_rec *r = pin_xmalloc(sizeof(*r));
    pin_buf_init(&r->buf);
    r->lvl = lvl;
    char ts[32];
    pin_iso8601_ms(ts, sizeof(ts));
    pin_buf_putc(&r->buf, '{');
    pin_buf_puts(&r->buf, "\"ts\":");
    pin_json_str(&r->buf, ts);
    pin_buf_puts(&r->buf, ",\"level\":");
    pin_json_str(&r->buf, level_name(lvl));
    pin_buf_puts(&r->buf, ",\"service\":");
    pin_json_str(&r->buf, g_log_service);
    pin_buf_puts(&r->buf, ",\"event\":");
    pin_json_str(&r->buf, event ? event : "unknown");
    return r;
}

static void emit_key(pin_log_rec *r, const char *k) {
    pin_buf_putc(&r->buf, ',');
    pin_json_str(&r->buf, k);
    pin_buf_putc(&r->buf, ':');
}

void pin_log_kv_str(pin_log_rec *r, const char *k, const char *v) {
    if (!r) return;
    emit_key(r, k);
    if (!v) {
        pin_buf_puts(&r->buf, "null");
        return;
    }
    /* Sanitize the value (kill ANSI / control chars from any source). */
    pin_buf clean;
    pin_buf_init(&clean);
    pin_text_sanitize(&clean, v, strlen(v), 4096);
    pin_json_str(&r->buf, clean.ptr ? clean.ptr : "");
    pin_buf_free(&clean);
}

void pin_log_kv_int(pin_log_rec *r, const char *k, long long v) {
    if (!r) return;
    emit_key(r, k);
    pin_buf_printf(&r->buf, "%lld", v);
}

void pin_log_kv_u64(pin_log_rec *r, const char *k, unsigned long long v) {
    if (!r) return;
    emit_key(r, k);
    pin_buf_printf(&r->buf, "%llu", v);
}

void pin_log_kv_bool(pin_log_rec *r, const char *k, int v) {
    if (!r) return;
    emit_key(r, k);
    pin_buf_puts(&r->buf, v ? "true" : "false");
}

void pin_log_kv_msgf(pin_log_rec *r, const char *fmt, ...) {
    if (!r) return;
    pin_buf tmp;
    pin_buf_init(&tmp);
    va_list ap;
    va_start(ap, fmt);
    pin_buf_vprintf(&tmp, fmt, ap);
    va_end(ap);
    pin_buf clean;
    pin_buf_init(&clean);
    pin_text_sanitize(&clean, tmp.ptr ? tmp.ptr : "", tmp.len, 4096);
    emit_key(r, "message");
    pin_json_str(&r->buf, clean.ptr ? clean.ptr : "");
    pin_buf_free(&clean);
    pin_buf_free(&tmp);
}

void pin_log_end(pin_log_rec *r) {
    if (!r) return;
    pin_buf_putc(&r->buf, '}');
    pin_buf_putc(&r->buf, '\n');
    pthread_mutex_lock(&g_log_mu);
    ssize_t n = write(STDERR_FILENO, r->buf.ptr, r->buf.len);
    (void)n;
    pthread_mutex_unlock(&g_log_mu);
    pin_buf_free(&r->buf);
    free(r);
}

void pin_log_simple(pin_log_level lvl, const char *event, const char *fmt, ...) {
    pin_log_rec *r = pin_log_begin(lvl, event);
    if (!r) return;
    if (fmt) {
        pin_buf tmp;
        pin_buf_init(&tmp);
        va_list ap;
        va_start(ap, fmt);
        pin_buf_vprintf(&tmp, fmt, ap);
        va_end(ap);
        pin_buf clean;
        pin_buf_init(&clean);
        pin_text_sanitize(&clean, tmp.ptr ? tmp.ptr : "", tmp.len, 4096);
        emit_key(r, "message");
        pin_json_str(&r->buf, clean.ptr ? clean.ptr : "");
        pin_buf_free(&clean);
        pin_buf_free(&tmp);
    }
    pin_log_end(r);
}
