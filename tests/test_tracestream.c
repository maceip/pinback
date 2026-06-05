#include "../src/tracestream.h"
#include "../src/util.h"

#include <stdio.h>
#include <string.h>

int run_tracestream_tests(void);

static int fails = 0;
#define EXPECT(c, m) do { \
    if (c) printf("ok  - %s\n", m); \
    else { printf("not ok - %s (%s:%d)\n", m, __FILE__, __LINE__); fails++; } \
} while (0)

typedef struct {
    char    answer[256];
    size_t  answer_len;
    char    thinking[256];
    size_t  thinking_len;
} trace_ud;

static void on_answer(void *ud, const char *t, size_t n) {
    trace_ud *u = ud;
    if (n >= sizeof(u->answer)) n = sizeof(u->answer) - 1;
    memcpy(u->answer, t, n);
    u->answer_len = n;
    u->answer[u->answer_len] = '\0';
}

static void on_thinking(void *ud, const char *t, size_t n) {
    trace_ud *u = ud;
    if (n >= sizeof(u->thinking)) n = sizeof(u->thinking) - 1;
    memcpy(u->thinking, t, n);
    u->thinking_len = n;
    u->thinking[u->thinking_len] = '\0';
}

static void test_generation_token(void) {
    trace_ud u = {0};
    pin_tracestream_cb cb = {
        .ud = &u, .on_answer = on_answer, .on_thinking = on_thinking,
    };
    pin_tracestream *ts = pin_tracestream_new(cb);
    EXPECT(ts != NULL, "tracestream opens");
    /* Through think-end so bytes count as answer, not thinking. */
    pin_tracestream_feed_line(ts,
        "token hex=3c2f72656461637465645f7468696e6b696e673e", 50);
    pin_tracestream_feed_line(ts, "token hex=4869", 14);
    pin_tracestream_flush(ts);
    EXPECT(u.answer_len == 2 && memcmp(u.answer, "Hi", 2) == 0,
           "generation hex decodes to answer prose");
    pin_tracestream_free(ts);
}

static void test_thinking_then_answer(void) {
    trace_ud u = {0};
    pin_tracestream_cb cb = {
        .ud = &u, .on_answer = on_answer, .on_thinking = on_thinking,
    };
    pin_tracestream *ts = pin_tracestream_new(cb);
    /* "ab" thinking, then end marker, then "Z" answer */
    pin_tracestream_feed_line(ts, "token hex=6162", 14);
    pin_tracestream_feed_line(ts, "token hex=3c2f72656461637465645f7468696e6b696e673e",
                              50);
    pin_tracestream_feed_line(ts, "token hex=5a", 12);
    pin_tracestream_flush(ts);
    EXPECT(u.thinking_len == 2 && memcmp(u.thinking, "ab", 2) == 0,
           "bytes before think-end are thinking");
    EXPECT(u.answer_len == 1 && u.answer[0] == 'Z', "bytes after think-end are answer");
    pin_tracestream_free(ts);
}

int run_tracestream_tests(void) {
    test_generation_token();
    test_thinking_then_answer();
    return fails;
}
