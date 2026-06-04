#include "tracestream.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* DSML tool-call block delimiters (fullwidth U+FF5C separators). */
#define DSML_OPEN  "<\xEF\xBD\x9C""DSML\xEF\xBD\x9C""tool_calls>"
#define DSML_CLOSE "</\xEF\xBD\x9C""DSML\xEF\xBD\x9C""tool_calls>"
#define THINK_END  "</think>"

/* Longest marker we must avoid splitting across a flush boundary. */
#define RESERVE 24

struct pin_tracestream {
    pin_tracestream_cb cb;
    int    skip;          /* prefill token lines still to skip */
    bool   answer_mode;   /* false until "</think>" seen this turn */
    bool   in_dsml;       /* inside a tool_calls block */
    pin_buf acc;          /* unprocessed generation bytes */
};

pin_tracestream *pin_tracestream_new(pin_tracestream_cb cb) {
    pin_tracestream *ts = calloc(1, sizeof(*ts));
    if (!ts) return NULL;
    ts->cb = cb;
    ts->answer_mode = false;
    pin_buf_init(&ts->acc);
    return ts;
}

void pin_tracestream_free(pin_tracestream *ts) {
    if (!ts) return;
    pin_buf_free(&ts->acc);
    free(ts);
}

static void consume(pin_buf *b, size_t n) {
    if (n >= b->len) { b->len = 0; b->ptr[0] = '\0'; return; }
    memmove(b->ptr, b->ptr + n, b->len - n);
    b->len -= n;
    b->ptr[b->len] = '\0';
}

/* Drain the generation accumulator into answer/thinking/tool_call events.
 * Complete markers anywhere in `acc` are always handled first; only a
 * possibly-partial trailing marker is held back (RESERVE bytes) unless
 * `flush` forces everything out. */
static void drain(pin_tracestream *ts, bool flush) {
    pin_buf *acc = &ts->acc;
    const pin_tracestream_cb *cb = &ts->cb;
    size_t reserve = flush ? 0 : RESERVE;
    for (;;) {
        if (acc->len == 0) break;
        if (!ts->answer_mode) {
            char *te = memmem(acc->ptr, acc->len, THINK_END, strlen(THINK_END));
            if (te) {
                size_t pre = (size_t)(te - acc->ptr);
                if (pre && cb->on_thinking) cb->on_thinking(cb->ud, acc->ptr, pre);
                consume(acc, pre + strlen(THINK_END));
                ts->answer_mode = true;
                continue;
            }
            size_t emit = acc->len > reserve ? acc->len - reserve : 0;
            if (emit && cb->on_thinking) cb->on_thinking(cb->ud, acc->ptr, emit);
            consume(acc, emit);
            break;
        }
        if (ts->in_dsml) {
            char *cl = memmem(acc->ptr, acc->len, DSML_CLOSE, strlen(DSML_CLOSE));
            if (cl) {
                size_t blk = (size_t)(cl - acc->ptr) + strlen(DSML_CLOSE);
                if (cb->on_tool_call) cb->on_tool_call(cb->ud, acc->ptr, blk);
                consume(acc, blk);
                ts->in_dsml = false;
                continue;
            }
            if (flush) {   /* unterminated block at stream end: emit as-is */
                if (cb->on_tool_call) cb->on_tool_call(cb->ud, acc->ptr, acc->len);
                consume(acc, acc->len);
                ts->in_dsml = false;
            }
            break;
        }
        char *op = memmem(acc->ptr, acc->len, DSML_OPEN, strlen(DSML_OPEN));
        if (op) {
            size_t pre = (size_t)(op - acc->ptr);
            if (pre && cb->on_answer) cb->on_answer(cb->ud, acc->ptr, pre);
            consume(acc, pre);          /* keep the open tag in acc */
            ts->in_dsml = true;
            continue;
        }
        size_t emit = acc->len > reserve ? acc->len - reserve : 0;
        if (emit && cb->on_answer) cb->on_answer(cb->ud, acc->ptr, emit);
        consume(acc, emit);
        break;
    }
}

/* Strip the "YYYY-MM-DD HH:MM:SS.mmm " timestamp prefix if present. */
static const char *strip_ts(const char *s, size_t *len) {
    if (*len > 24 && s[4] == '-' && s[7] == '-' && s[10] == ' ' &&
        s[13] == ':' && s[16] == ':') {
        const char *sp = memchr(s + 19, ' ', *len - 19);
        if (sp) { size_t adv = (size_t)(sp - s) + 1; *len -= adv; return s + adv; }
    }
    return s;
}

/* Parse `hex=...` into the accumulator (generation token bytes). */
static void append_hex(pin_buf *acc, const char *p, size_t n) {
    for (size_t i = 0; i + 1 < n; i += 2) {
        int hi = p[i], lo = p[i + 1];
        if (!isxdigit(hi) || !isxdigit(lo)) break;
        int v = (isdigit(hi) ? hi - '0' : (tolower(hi) - 'a' + 10)) * 16 +
                (isdigit(lo) ? lo - '0' : (tolower(lo) - 'a' + 10));
        pin_buf_putc(acc, (char)v);
    }
}

void pin_tracestream_feed_line(pin_tracestream *ts, const char *line, size_t len) {
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    const char *s = strip_ts(line, &len);

    if (len >= 12 && memcmp(s, "tokens label", 12) == 0) {
        long a = -1, b = -1;
        const char *st = strstr(s, "start=");
        const char *ln = strstr(s, "len=");
        if (st) a = strtol(st + 6, NULL, 10);
        if (ln) b = strtol(ln + 4, NULL, 10);
        if (a >= 0 && b >= a) ts->skip = (int)(b - a);
        return;
    }
    if (len >= 5 && memcmp(s, "user=", 5) == 0) {
        /* new user turn: reset per-turn parse state */
        ts->answer_mode = false;
        ts->in_dsml = false;
        ts->acc.len = 0;
        if (ts->acc.ptr) ts->acc.ptr[0] = '\0';
        if (ts->cb.on_turn) ts->cb.on_turn(ts->cb.ud);
        return;
    }
    if (len >= 6 && memcmp(s, "token ", 6) == 0) {
        if (ts->skip > 0) { ts->skip--; return; }   /* prefill token: drop */
        const char *hx = strstr(s, "hex=");
        if (hx) {
            const char *h = hx + 4;
            size_t hn = len - (size_t)(h - s);
            append_hex(&ts->acc, h, hn);
            drain(ts, false);
        }
        return;
    }
    /* other structural lines (prefill/dsml/datetime): boundaries; flush
     * what we safely can so prose does not stall behind the reserve. */
    drain(ts, false);
}

void pin_tracestream_flush(pin_tracestream *ts) {
    drain(ts, true);
}
