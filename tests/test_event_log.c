#include "../src/event_log.h"
#include "../src/util.h"

int run_event_log_tests(void);

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

static char *unique_path(void) {
    char *p = pin_xmalloc(64);
    snprintf(p, 64, "/tmp/pinback-test-%d-%lld.log",
             (int)getpid(), (long long)pin_monotonic_ms());
    return p;
}

static void test_open_append_status(void) {
    char *path = unique_path();
    unlink(path);
    pin_event_log *log = pin_event_log_open(path, 8);
    EXPECT(log != NULL, "event log opens");
    pin_event_log_status st;
    pin_event_log_status_get(log, &st);
    EXPECT(st.newest_seq == 0, "newest_seq == 0 initially");
    EXPECT(st.generation >= 1, "generation >= 1");

    pin_event_log_append(log, "user", "{\"text\":\"hi\"}", 13);
    pin_event_log_status_get(log, &st);
    EXPECT(st.newest_seq == 1, "newest_seq == 1 after append");
    EXPECT(st.ring_used == 1, "ring_used 1");

    /* Append more than ring_capacity to test eviction. */
    for (int i = 0; i < 12; i++) {
        char body[32];
        int n = snprintf(body, sizeof(body), "{\"i\":%d}", i);
        pin_event_log_append(log, "answer", body, (size_t)n);
    }
    pin_event_log_status_get(log, &st);
    EXPECT(st.ring_used == 8, "ring used == cap after eviction");
    EXPECT(st.newest_seq == 13, "newest_seq advanced");
    EXPECT(st.oldest_seq == st.newest_seq - 7, "oldest_seq aligned to ring");

    pin_event_log_close(log);
    /* Reopen and confirm replay loads previous state. */
    pin_event_log *log2 = pin_event_log_open(path, 8);
    pin_event_log_status_get(log2, &st);
    EXPECT(st.newest_seq == 13, "replay restores newest_seq");
    EXPECT(st.generation >= 2, "generation bumps on reopen");
    pin_event_log_close(log2);
    unlink(path);
    free(path);
}

/* Subscriber thread: reads all SSE bytes from a socketpair until EOF or
 * the log closes. We don't parse — we just count `data:` lines. */
typedef struct {
    pin_event_log *log;
    int            fd;
    int            data_lines;
} sub_thread_args;

static void *sub_thread(void *arg) {
    sub_thread_args *a = arg;
    pin_subscriber_callbacks cb = {0};
    pin_event_log_serve_subscriber(a->log, a->fd, PIN_SUB_KIND_SSE, -1, -1, cb);
    return NULL;
}

typedef struct {
    pin_event_log *log;
    int            fd;
    long long      client_generation;
} sub_gen_args;

static void *sub_thread_gen(void *arg) {
    sub_gen_args *a = arg;
    pin_subscriber_callbacks cb = {0};
    pin_event_log_serve_subscriber(a->log, a->fd, PIN_SUB_KIND_SSE,
                                   a->client_generation, -1, cb);
    return NULL;
}

static int count_data_lines(const char *buf, size_t len) {
    int n = 0;
    for (size_t i = 0; i + 5 < len; i++) {
        if (memcmp(buf + i, "\ndata:", 6) == 0) n++;
    }
    /* Initial line at offset 0 has no preceding \n. */
    if (len > 5 && memcmp(buf, "data:", 5) == 0) n++;
    return n;
}

static void test_fanout(void) {
    char *path = unique_path();
    pin_event_log *log = pin_event_log_open(path, 32);

    int s1[2], s2[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, s1);
    socketpair(AF_UNIX, SOCK_STREAM, 0, s2);

    sub_thread_args a1 = {.log = log, .fd = s1[1]};
    sub_thread_args a2 = {.log = log, .fd = s2[1]};
    pthread_t t1, t2;
    pthread_create(&t1, NULL, sub_thread, &a1);
    pthread_create(&t2, NULL, sub_thread, &a2);

    /* Give subscribers a moment to attach. */
    usleep(50 * 1000);

    /* Append a few events. */
    pin_event_log_append(log, "user",   "{\"text\":\"a\"}", 12);
    pin_event_log_append(log, "answer", "{\"text\":\"x\"}", 12);
    pin_event_log_append(log, "answer_end", "{\"reason\":\"stop\"}", 17);

    /* Drain. */
    usleep(100 * 1000);

    /* Read what subscribers received from our end. */
    char buf1[4096], buf2[4096];
    ssize_t n1 = recv(s1[0], buf1, sizeof(buf1), MSG_DONTWAIT);
    ssize_t n2 = recv(s2[0], buf2, sizeof(buf2), MSG_DONTWAIT);
    EXPECT(n1 > 0 && n2 > 0, "both subscribers received bytes");
    int c1 = count_data_lines(buf1, (size_t)(n1 > 0 ? n1 : 0));
    int c2 = count_data_lines(buf2, (size_t)(n2 > 0 ? n2 : 0));
    EXPECT(c1 == 3 && c2 == 3, "both subscribers see all 3 events");

    /* Close: the serve loops will exit when the socket is shut. */
    close(s1[0]); close(s2[0]);
    close(s1[1]); close(s2[1]);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pin_event_log_close(log);
    unlink(path); free(path);
}

static void test_generation_mismatch(void) {
    char *path = unique_path();
    unlink(path);
    pin_event_log *log = pin_event_log_open(path, 8);
    pin_event_log_status st;
    pin_event_log_status_get(log, &st);
    long long gen1 = st.generation;
    pin_event_log_append(log, "user", "{\"text\":\"a\"}", 12);
    pin_event_log_close(log);

    pin_event_log *log2 = pin_event_log_open(path, 8);
    pin_event_log_status_get(log2, &st);
    EXPECT(st.generation > gen1, "generation bumps on reopen");

    int sp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    sub_gen_args ga = {.log = log2, .fd = sp[1], .client_generation = gen1};
    pthread_t t;
    pthread_create(&t, NULL, sub_thread_gen, &ga);
    char buf[8192];
    size_t total = 0;
    bool saw = false;
    for (int i = 0; i < 100; i++) {
        usleep(20 * 1000);
        ssize_t n = recv(sp[0], buf + total, sizeof(buf) - total, MSG_DONTWAIT);
        if (n > 0) {
            total += (size_t)n;
            if (total < sizeof(buf)) buf[total] = '\0';
            if (strstr(buf, "generation_mismatch") != NULL) {
                saw = true;
                break;
            }
        }
    }
    EXPECT(saw, "stale client generation emits generation_mismatch");

    close(sp[0]);
    shutdown(sp[1], SHUT_RDWR);
    close(sp[1]);
    pthread_join(t, NULL);
    pin_event_log_close(log2);
    unlink(path);
    free(path);
}

int run_event_log_tests(void) {
    test_open_append_status();
    test_fanout();
    test_generation_mismatch();
    return fails;
}
