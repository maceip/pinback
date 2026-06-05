#ifndef PIN_LOG_H
#define PIN_LOG_H

/* JSON Lines structured logger. One write(2) per record, thread-safe.
 *
 * Every record is a single JSON object on stderr (or a configured fp)
 * with at minimum:
 *   ts, level, service, event
 *
 * Other fields are appended via pin_log_kv_*. The required event names
 * are defined as PIN_EV_* constants below to prevent typos.
 *
 * All string values are pin_text_sanitize'd before emission: a stray
 * ANSI escape from a child process can never poison our log file. */

#include <stdarg.h>
#include <stddef.h>

typedef enum {
    PIN_LOG_DEBUG = 10,
    PIN_LOG_INFO = 20,
    PIN_LOG_WARN = 30,
    PIN_LOG_ERROR = 40,
} pin_log_level;

void pin_log_init(const char *service, pin_log_level min_level);
void pin_log_set_min_level(pin_log_level lvl);

typedef struct pin_log_rec pin_log_rec;

pin_log_rec *pin_log_begin(pin_log_level lvl, const char *event);
void pin_log_kv_str(pin_log_rec *r, const char *k, const char *v);
void pin_log_kv_int(pin_log_rec *r, const char *k, long long v);
void pin_log_kv_u64(pin_log_rec *r, const char *k, unsigned long long v);
void pin_log_kv_bool(pin_log_rec *r, const char *k, int v);
void pin_log_kv_msgf(pin_log_rec *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void pin_log_end(pin_log_rec *r);

/* Convenience: one-line call with optional message. */
void pin_log_simple(pin_log_level lvl, const char *event, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define PIN_LOG_INFOF(EV, ...) pin_log_simple(PIN_LOG_INFO, (EV), __VA_ARGS__)
#define PIN_LOG_WARNF(EV, ...) pin_log_simple(PIN_LOG_WARN, (EV), __VA_ARGS__)
#define PIN_LOG_ERRF(EV, ...) pin_log_simple(PIN_LOG_ERROR, (EV), __VA_ARGS__)
#define PIN_LOG_DEBUGF(EV, ...) pin_log_simple(PIN_LOG_DEBUG, (EV), __VA_ARGS__)

/* Required event names (per ARCHITECTURE_REDO.md). Use these constants
 * everywhere so a quick `rg PIN_EV_` shows the full event surface. */
#define PIN_EV_BOOT_LISTEN "boot.listen"
#define PIN_EV_BOOT_SHUTDOWN "boot.shutdown"
#define PIN_EV_DS4_PROC_SPAWN "ds4.proc.spawn"
#define PIN_EV_DS4_PROC_READY "ds4.proc.ready"
#define PIN_EV_DS4_PROC_EXIT "ds4.proc.exit"
#define PIN_EV_UPSTREAM_CONNECT "upstream.connect"
#define PIN_EV_UPSTREAM_DISCONNECT "upstream.disconnect"
#define PIN_EV_INPUT_ACCEPTED "input.accepted"
#define PIN_EV_INPUT_REJECTED "input.rejected"
#define PIN_EV_STREAM_OPEN "stream.open"
#define PIN_EV_STREAM_RESUME "stream.resume"
#define PIN_EV_STREAM_CURSOR_RESET "stream.cursor_reset"
#define PIN_EV_STREAM_SNAPSHOT_REQUIRED "stream.snapshot_required"
#define PIN_EV_STREAM_CLOSE "stream.close"
#define PIN_EV_CONTROL_REQUEST "control.request"
#define PIN_EV_CONTROL_RESULT "control.result"
#define PIN_EV_INGRESS_UNHEALTHY "ingress.unhealthy"
#define PIN_EV_HTTP_REQUEST "http.request"
#define PIN_EV_HTTP_REJECT "http.reject"

#endif
