#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static void pin_die(const char *msg);

/* ------------------------------------------------------------------ *
 * pin_buf                                                            *
 * ------------------------------------------------------------------ */

void pin_buf_init(pin_buf *b)
{
    b->ptr = NULL;
    b->len = 0;
    b->cap = 0;
}

void pin_buf_reserve(pin_buf *b, size_t add)
{
    if (add > SIZE_MAX - b->len - 1)
        pin_die("pinback: buffer overflow");
    if (b->len + add + 1 <= b->cap)
        return;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->len + add + 1)
        nc *= 2;
    b->ptr = pin_xrealloc(b->ptr, nc);
    b->cap = nc;
}

void pin_buf_append(pin_buf *b, const void *p, size_t n)
{
    if (n == 0)
        return;
    pin_buf_reserve(b, n);
    memcpy(b->ptr + b->len, p, n);
    b->len += n;
    b->ptr[b->len] = '\0';
}

void pin_buf_putc(pin_buf *b, char c)
{
    pin_buf_append(b, &c, 1);
}

void pin_buf_puts(pin_buf *b, const char *s)
{
    if (s)
        pin_buf_append(b, s, strlen(s));
}

void pin_buf_vprintf(pin_buf *b, const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0)
        return;
    pin_buf_reserve(b, (size_t)n);
    int wrote = vsnprintf(b->ptr + b->len, b->cap - b->len, fmt, ap);
    if (wrote > 0)
        b->len += (size_t)wrote;
    b->ptr[b->len] = '\0';
}

void pin_buf_printf(pin_buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pin_buf_vprintf(b, fmt, ap);
    va_end(ap);
}

void pin_buf_clear(pin_buf *b)
{
    b->len = 0;
    if (b->ptr)
        b->ptr[0] = '\0';
}

void pin_buf_free(pin_buf *b)
{
    free(b->ptr);
    b->ptr = NULL;
    b->len = b->cap = 0;
}

char *pin_buf_detach(pin_buf *b)
{
    char *p = b->ptr;
    b->ptr = NULL;
    b->len = b->cap = 0;
    return p;
}

/* ------------------------------------------------------------------ *
 * Allocators                                                         *
 * ------------------------------------------------------------------ */

static void pin_die(const char *msg)
{
    /* No log dependency here. Write directly. */
    if (write(STDERR_FILENO, msg, strlen(msg)) < 0) { /* nothing */
    }
    if (write(STDERR_FILENO, "\n", 1) < 0) { /* nothing */
    }
    abort();
}

void *pin_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        pin_die("pinback: out of memory");
    return p;
}

void *pin_xcalloc(size_t n, size_t m)
{
    void *p = calloc(n ? n : 1, m ? m : 1);
    if (!p)
        pin_die("pinback: out of memory");
    return p;
}

void *pin_xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        pin_die("pinback: out of memory");
    return q;
}

char *pin_xstrdup(const char *s)
{
    if (!s)
        return NULL;
    size_t n = strlen(s);
    char *p = pin_xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char *pin_xstrndup(const char *s, size_t n)
{
    char *p = pin_xmalloc(n + 1);
    if (n)
        memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* ------------------------------------------------------------------ *
 * Time                                                               *
 * ------------------------------------------------------------------ */

uint64_t pin_wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
}

uint64_t pin_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
}

void pin_iso8601_ms(char *out, size_t cap)
{
    if (cap < 32) {
        if (cap > 0)
            out[0] = '\0';
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    int n =
        snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ", tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    if (n < 0 || (size_t)n >= cap)
        out[0] = '\0';
}

/* ------------------------------------------------------------------ *
 * Strings                                                            *
 * ------------------------------------------------------------------ */

bool pin_iequals(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    return strcasecmp(a, b) == 0;
}

bool pin_istarts_with(const char *s, const char *prefix)
{
    if (!s || !prefix)
        return false;
    size_t n = strlen(prefix);
    return strncasecmp(s, prefix, n) == 0;
}

void pin_random_hex(char *out, size_t out_cap, size_t bytes)
{
    static const char hex[] = "0123456789abcdef";
    if (out_cap < bytes * 2 + 1) {
        if (out_cap > 0)
            out[0] = '\0';
        return;
    }
    uint8_t buf[64];
    if (bytes > sizeof(buf))
        bytes = sizeof(buf);
    int fd = open("/dev/urandom", O_RDONLY);
    bool ok = false;
    if (fd >= 0) {
        ssize_t n = read(fd, buf, bytes);
        close(fd);
        ok = (n == (ssize_t)bytes);
    }
    if (!ok) {
        /* Fallback. Not crypto, but request_id collisions are tolerable. */
        uint64_t t = pin_monotonic_ms();
        for (size_t i = 0; i < bytes; i++) {
            buf[i] = (uint8_t)((t ^ (t >> (8 * (i % 8)))) ^ (i * 131));
        }
    }
    for (size_t i = 0; i < bytes; i++) {
        out[i * 2] = hex[(buf[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[buf[i] & 0xF];
    }
    out[bytes * 2] = '\0';
}

/* ------------------------------------------------------------------ *
 * UTF-8 + sanitizer                                                  *
 * ------------------------------------------------------------------ */

/* Length of a UTF-8 sequence given its leading byte; 0 = invalid. */
static int utf8_seq_len(uint8_t b0)
{
    if ((b0 & 0x80) == 0x00)
        return 1;
    if ((b0 & 0xE0) == 0xC0)
        return 2;
    if ((b0 & 0xF0) == 0xE0)
        return 3;
    if ((b0 & 0xF8) == 0xF0)
        return 4;
    return 0;
}

/* Decode one UTF-8 codepoint at p (len bytes available). On success
 * returns the byte count consumed and writes *cp; on failure returns 0. */
static int utf8_decode(const uint8_t *p, size_t len, uint32_t *cp)
{
    if (len == 0)
        return 0;
    int n = utf8_seq_len(p[0]);
    if (n == 0 || (size_t)n > len)
        return 0;
    uint32_t v;
    switch (n) {
    case 1:
        v = p[0];
        break;
    case 2:
        if ((p[1] & 0xC0) != 0x80)
            return 0;
        v = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        if (v < 0x80)
            return 0; /* overlong */
        break;
    case 3:
        if ((p[1] & 0xC0) != 0x80)
            return 0;
        if ((p[2] & 0xC0) != 0x80)
            return 0;
        v = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
            ((uint32_t)(p[2] & 0x3F));
        if (v < 0x800)
            return 0; /* overlong */
        if (v >= 0xD800 && v <= 0xDFFF)
            return 0; /* surrogate */
        break;
    case 4:
        if ((p[1] & 0xC0) != 0x80)
            return 0;
        if ((p[2] & 0xC0) != 0x80)
            return 0;
        if ((p[3] & 0xC0) != 0x80)
            return 0;
        v = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
            ((uint32_t)(p[2] & 0x3F) << 6) | ((uint32_t)(p[3] & 0x3F));
        if (v < 0x10000 || v > 0x10FFFF)
            return 0;
        break;
    default:
        return 0;
    }
    *cp = v;
    return n;
}

static void utf8_encode(pin_buf *out, uint32_t cp)
{
    char tmp[4];
    if (cp < 0x80) {
        tmp[0] = (char)cp;
        pin_buf_append(out, tmp, 1);
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        pin_buf_append(out, tmp, 2);
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        pin_buf_append(out, tmp, 3);
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F));
        pin_buf_append(out, tmp, 4);
    }
}

bool pin_text_sanitize(pin_buf *out, const char *in, size_t len, size_t cap)
{
    const uint8_t *p = (const uint8_t *)in;
    const uint8_t *end = p + len;
    size_t before = out->len;
    bool ok = true;
    while (p < end) {
        if (out->len - before >= cap) {
            ok = false;
            break;
        }
        uint32_t cp;
        int n = utf8_decode(p, (size_t)(end - p), &cp);
        if (n == 0) {
            /* Invalid: skip one byte, emit U+FFFD, resync. */
            utf8_encode(out, 0xFFFD);
            p++;
            continue;
        }
        /* Drop C0 except \t and \n. */
        if (cp < 0x20 && cp != '\t' && cp != '\n') {
            p += n;
            continue;
        }
        /* Drop standalone C1 (also covers DEL=0x7F? We allow 0x7F since
         * it's harmless in UTF-8 text and is not an ANSI initiator). */
        if (cp >= 0x80 && cp <= 0x9F) {
            p += n;
            continue;
        }
        utf8_encode(out, cp);
        p += n;
    }
    return ok;
}

/* ------------------------------------------------------------------ *
 * JSON emit                                                          *
 * ------------------------------------------------------------------ */

void pin_json_strn(pin_buf *b, const char *s, size_t len)
{
    pin_buf_putc(b, '"');
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':
            pin_buf_puts(b, "\\\"");
            break;
        case '\\':
            pin_buf_puts(b, "\\\\");
            break;
        case '\b':
            pin_buf_puts(b, "\\b");
            break;
        case '\f':
            pin_buf_puts(b, "\\f");
            break;
        case '\n':
            pin_buf_puts(b, "\\n");
            break;
        case '\r':
            pin_buf_puts(b, "\\r");
            break;
        case '\t':
            pin_buf_puts(b, "\\t");
            break;
        default:
            if (c < 0x20) {
                char esc[7] = "\\u00..";
                esc[4] = hex[(c >> 4) & 0xF];
                esc[5] = hex[c & 0xF];
                pin_buf_append(b, esc, 6);
            } else {
                pin_buf_putc(b, (char)c);
            }
        }
    }
    pin_buf_putc(b, '"');
}

void pin_json_str(pin_buf *b, const char *s)
{
    if (!s) {
        pin_buf_puts(b, "null");
        return;
    }
    pin_json_strn(b, s, strlen(s));
}

/* ------------------------------------------------------------------ *
 * JSON parse (narrow, mirrors ds4_server.c)                          *
 * ------------------------------------------------------------------ */

void pin_json_ws(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')
        (*p)++;
}

static int hexdig(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static void utf8_put(pin_buf *b, uint32_t cp)
{
    utf8_encode(b, cp);
}

bool pin_json_parse_string(const char **p, char **out)
{
    pin_json_ws(p);
    if (**p != '"')
        return false;
    (*p)++;
    pin_buf b;
    pin_buf_init(&b);
    while (**p && **p != '"') {
        if (**p == '\\') {
            (*p)++;
            switch (**p) {
            case '"':
                pin_buf_putc(&b, '"');
                (*p)++;
                break;
            case '\\':
                pin_buf_putc(&b, '\\');
                (*p)++;
                break;
            case '/':
                pin_buf_putc(&b, '/');
                (*p)++;
                break;
            case 'b':
                pin_buf_putc(&b, '\b');
                (*p)++;
                break;
            case 'f':
                pin_buf_putc(&b, '\f');
                (*p)++;
                break;
            case 'n':
                pin_buf_putc(&b, '\n');
                (*p)++;
                break;
            case 'r':
                pin_buf_putc(&b, '\r');
                (*p)++;
                break;
            case 't':
                pin_buf_putc(&b, '\t');
                (*p)++;
                break;
            case 'u': {
                (*p)++;
                int h0 = hexdig((*p)[0]), h1 = hexdig((*p)[1]);
                int h2 = hexdig((*p)[2]), h3 = hexdig((*p)[3]);
                if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) {
                    pin_buf_free(&b);
                    return false;
                }
                uint32_t cp = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                (*p) += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && (*p)[0] == '\\' && (*p)[1] == 'u') {
                    int g0 = hexdig((*p)[2]), g1 = hexdig((*p)[3]);
                    int g2 = hexdig((*p)[4]), g3 = hexdig((*p)[5]);
                    if (g0 >= 0 && g1 >= 0 && g2 >= 0 && g3 >= 0) {
                        uint32_t lo = (uint32_t)((g0 << 12) | (g1 << 8) | (g2 << 4) | g3);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                            (*p) += 6;
                        }
                    }
                }
                utf8_put(&b, cp);
                break;
            }
            default:
                pin_buf_free(&b);
                return false;
            }
        } else {
            pin_buf_putc(&b, **p);
            (*p)++;
        }
    }
    if (**p != '"') {
        pin_buf_free(&b);
        return false;
    }
    (*p)++;
    *out = pin_buf_detach(&b);
    if (!*out)
        *out = pin_xstrdup("");
    return true;
}

bool pin_json_parse_bool(const char **p, bool *out)
{
    pin_json_ws(p);
    if (!strncmp(*p, "true", 4)) {
        *p += 4;
        *out = true;
        return true;
    }
    if (!strncmp(*p, "false", 5)) {
        *p += 5;
        *out = false;
        return true;
    }
    return false;
}

bool pin_json_parse_int(const char **p, long long *out)
{
    pin_json_ws(p);
    char *end = NULL;
    long long v = strtoll(*p, &end, 10);
    if (end == *p)
        return false;
    *p = end;
    *out = v;
    return true;
}

bool pin_json_parse_double(const char **p, double *out)
{
    pin_json_ws(p);
    char *end = NULL;
    double v = strtod(*p, &end);
    if (end == *p)
        return false;
    *p = end;
    *out = v;
    return true;
}

static bool skip_string(const char **p)
{
    if (**p != '"')
        return false;
    (*p)++;
    while (**p && **p != '"') {
        if (**p == '\\' && (*p)[1])
            (*p) += 2;
        else
            (*p)++;
    }
    if (**p != '"')
        return false;
    (*p)++;
    return true;
}

static bool skip_value_depth(const char **p, int depth)
{
    if (depth > 64)
        return false;
    pin_json_ws(p);
    if (**p == '"')
        return skip_string(p);
    if (**p == '{') {
        (*p)++;
        pin_json_ws(p);
        while (**p && **p != '}') {
            if (!skip_string(p))
                return false;
            pin_json_ws(p);
            if (**p != ':')
                return false;
            (*p)++;
            if (!skip_value_depth(p, depth + 1))
                return false;
            pin_json_ws(p);
            if (**p == ',') {
                (*p)++;
                pin_json_ws(p);
                continue;
            }
            if (**p != '}')
                return false;
        }
        if (**p != '}')
            return false;
        (*p)++;
        return true;
    }
    if (**p == '[') {
        (*p)++;
        pin_json_ws(p);
        while (**p && **p != ']') {
            if (!skip_value_depth(p, depth + 1))
                return false;
            pin_json_ws(p);
            if (**p == ',') {
                (*p)++;
                pin_json_ws(p);
                continue;
            }
            if (**p != ']')
                return false;
        }
        if (**p != ']')
            return false;
        (*p)++;
        return true;
    }
    if (!strncmp(*p, "true", 4)) {
        *p += 4;
        return true;
    }
    if (!strncmp(*p, "false", 5)) {
        *p += 5;
        return true;
    }
    if (!strncmp(*p, "null", 4)) {
        *p += 4;
        return true;
    }
    /* number */
    char *end = NULL;
    (void)strtod(*p, &end);
    if (end == *p)
        return false;
    *p = end;
    return true;
}

bool pin_json_skip_value(const char **p)
{
    return skip_value_depth(p, 0);
}

bool pin_json_find_key(const char *obj, const char *key, const char **out)
{
    if (!obj || *obj != '{')
        return false;
    const char *p = obj + 1;
    while (*p) {
        pin_json_ws(&p);
        if (*p == '}' || !*p)
            return false;
        char *k = NULL;
        if (!pin_json_parse_string(&p, &k))
            return false;
        pin_json_ws(&p);
        if (*p != ':') {
            free(k);
            return false;
        }
        p++;
        pin_json_ws(&p);
        if (!strcmp(k, key)) {
            free(k);
            *out = p;
            return true;
        }
        free(k);
        if (!pin_json_skip_value(&p))
            return false;
        pin_json_ws(&p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}')
            return false;
    }
    return false;
}

/* ------------------------------------------------------------------ *
 * SHA-1 (public domain, Steve Reid 1995, transcribed)                *
 * ------------------------------------------------------------------ */

static uint32_t rol32(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

static void sha1_transform(pin_sha1 *c, const uint8_t b[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16) |
               ((uint32_t)b[i * 4 + 2] << 8) | ((uint32_t)b[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = c->state[0], B = c->state[1], C = c->state[2];
    uint32_t d = c->state[3], e = c->state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (B & C) | ((~B) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = B ^ C ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (B & C) | (B & d) | (C & d);
            k = 0x8F1BBCDC;
        } else {
            f = B ^ C ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = C;
        C = rol32(B, 30);
        B = a;
        a = t;
    }
    c->state[0] += a;
    c->state[1] += B;
    c->state[2] += C;
    c->state[3] += d;
    c->state[4] += e;
}

void pin_sha1_init(pin_sha1 *c)
{
    c->state[0] = 0x67452301;
    c->state[1] = 0xEFCDAB89;
    c->state[2] = 0x98BADCFE;
    c->state[3] = 0x10325476;
    c->state[4] = 0xC3D2E1F0;
    c->count = 0;
    c->used = 0;
}

void pin_sha1_update(pin_sha1 *c, const void *p, size_t n)
{
    const uint8_t *b = p;
    c->count += n;
    while (n) {
        size_t take = 64 - c->used;
        if (take > n)
            take = n;
        memcpy(c->block + c->used, b, take);
        c->used += take;
        b += take;
        n -= take;
        if (c->used == 64) {
            sha1_transform(c, c->block);
            c->used = 0;
        }
    }
}

void pin_sha1_final(pin_sha1 *c, uint8_t out[20])
{
    uint64_t bits = c->count * 8;
    uint8_t one = 0x80, zero = 0;
    pin_sha1_update(c, &one, 1);
    while (c->used != 56)
        pin_sha1_update(c, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (56 - i * 8));
    pin_sha1_update(c, len, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(c->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->state[i]);
    }
}

void pin_sha1_bytes(const void *p, size_t n, uint8_t out[20])
{
    pin_sha1 c;
    pin_sha1_init(&c);
    pin_sha1_update(&c, p, n);
    pin_sha1_final(&c, out);
}

void pin_sha1_hex(const void *p, size_t n, char out[41])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t d[20];
    pin_sha1_bytes(p, n, d);
    for (int i = 0; i < 20; i++) {
        out[i * 2] = hex[d[i] >> 4];
        out[i * 2 + 1] = hex[d[i] & 0xF];
    }
    out[40] = '\0';
}

/* ------------------------------------------------------------------ *
 * base64 — fixed 20-byte encode (used by WebSocket Sec-Accept)       *
 * ------------------------------------------------------------------ */

void pin_base64_20(const uint8_t in[20], char out[29])
{
    static const char alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    /* 20 input bytes -> 28 output chars + '=' padding to length 28
     * (because (20*8) / 6 = 26.67; 28 chars cover it with one '=' pad).
     * Actually: ceil(20/3)*4 = 28. Last group is 2 input bytes -> 3
     * output chars + '='. */
    int o = 0;
    int i = 0;
    for (; i + 3 <= 20; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = alpha[(v >> 18) & 0x3F];
        out[o++] = alpha[(v >> 12) & 0x3F];
        out[o++] = alpha[(v >> 6) & 0x3F];
        out[o++] = alpha[v & 0x3F];
    }
    /* remaining 2 bytes */
    if (i + 2 == 20) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = alpha[(v >> 18) & 0x3F];
        out[o++] = alpha[(v >> 12) & 0x3F];
        out[o++] = alpha[(v >> 6) & 0x3F];
        out[o++] = '=';
    }
    out[o] = '\0';
}
