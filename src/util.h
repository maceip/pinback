#ifndef PIN_UTIL_H
#define PIN_UTIL_H

/* Small primitives used everywhere.
 *
 * Style mirrors ds4_server.c: growable byte buffer, oom-aborting
 * allocators, a hand-rolled JSON emitter and parser narrow to the
 * fields we use, plus SHA-1 and base64 vendored so we do not link
 * against ds4 internals or pull in a crypto library. Nothing here
 * depends on any other pinback module. */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ *
 * Growable byte buffer                                               *
 * ------------------------------------------------------------------ */

typedef struct {
    char  *ptr;
    size_t len;
    size_t cap;
} pin_buf;

void  pin_buf_init(pin_buf *b);
void  pin_buf_reserve(pin_buf *b, size_t add);
void  pin_buf_append(pin_buf *b, const void *p, size_t n);
void  pin_buf_putc(pin_buf *b, char c);
void  pin_buf_puts(pin_buf *b, const char *s);
void  pin_buf_printf(pin_buf *b, const char *fmt, ...)
        __attribute__((format(printf, 2, 3)));
void  pin_buf_vprintf(pin_buf *b, const char *fmt, va_list ap);
void  pin_buf_clear(pin_buf *b);
void  pin_buf_free(pin_buf *b);
char *pin_buf_detach(pin_buf *b);

/* ------------------------------------------------------------------ *
 * Allocators (oom -> abort with diagnostic)                          *
 * ------------------------------------------------------------------ */

void *pin_xmalloc(size_t n);
void *pin_xcalloc(size_t n, size_t m);
void *pin_xrealloc(void *p, size_t n);
char *pin_xstrdup(const char *s);
char *pin_xstrndup(const char *s, size_t n);

/* ------------------------------------------------------------------ *
 * Time                                                               *
 * ------------------------------------------------------------------ */

uint64_t pin_wall_ms(void);                       /* Unix ms */
uint64_t pin_monotonic_ms(void);                  /* CLOCK_MONOTONIC ms */
void     pin_iso8601_ms(char *out, size_t cap);   /* "..Z", cap >= 32 */

/* ------------------------------------------------------------------ *
 * Strings                                                            *
 * ------------------------------------------------------------------ */

bool pin_iequals(const char *a, const char *b);
bool pin_istarts_with(const char *s, const char *prefix);

/* Random hex id: out gets 2*bytes hex chars + NUL. out_cap >= 2*bytes+1. */
void pin_random_hex(char *out, size_t out_cap, size_t bytes);

/* ------------------------------------------------------------------ *
 * Untrusted-text sanitizer                                           *
 * ------------------------------------------------------------------ */

/* Sanitize untrusted text bytes for inclusion in events / logs.
 *
 *   - drops C0 control chars 0x00..0x1F except \t (0x09) and \n (0x0A);
 *     this kills ANSI by construction since ESC=0x1B is in C0
 *   - drops C1 control chars 0x80..0x9F when they appear standalone;
 *     valid UTF-8 that happens to encode those code points is dropped
 *   - replaces invalid UTF-8 sequences with U+FFFD
 *
 * Output is appended to `out`. Returns true if all input fit; returns
 * false if `cap` was hit (caller should split into multiple events). */
bool pin_text_sanitize(pin_buf *out, const char *in, size_t len, size_t cap);

/* ------------------------------------------------------------------ *
 * JSON emit                                                          *
 * ------------------------------------------------------------------ */

/* Emit "..." with full escaping: ", \, all control chars 0x00..0x1F as
 * \u00XX. UTF-8 (>=0x20) passes through. */
void pin_json_str(pin_buf *b, const char *s);
void pin_json_strn(pin_buf *b, const char *s, size_t len);

/* ------------------------------------------------------------------ *
 * JSON parse (narrow, mirrors ds4_server.c)                          *
 * ------------------------------------------------------------------ */

void pin_json_ws(const char **p);
bool pin_json_parse_string(const char **p, char **out);
bool pin_json_parse_bool(const char **p, bool *out);
bool pin_json_parse_int(const char **p, long long *out);
bool pin_json_parse_double(const char **p, double *out);
bool pin_json_skip_value(const char **p);
/* obj must point at '{'. On hit, *out is positioned at the value. */
bool pin_json_find_key(const char *obj, const char *key, const char **out);

/* ------------------------------------------------------------------ *
 * SHA-1 (public domain) + base64                                     *
 * ------------------------------------------------------------------ */

typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t  block[64];
    size_t   used;
} pin_sha1;

void pin_sha1_init(pin_sha1 *c);
void pin_sha1_update(pin_sha1 *c, const void *p, size_t n);
void pin_sha1_final(pin_sha1 *c, uint8_t out[20]);
void pin_sha1_bytes(const void *p, size_t n, uint8_t out[20]);
void pin_sha1_hex(const void *p, size_t n, char out[41]);

/* base64-encode exactly 20 raw bytes into 28 chars + NUL. */
void pin_base64_20(const uint8_t in[20], char out[29]);

#endif
