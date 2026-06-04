#include "../src/agent.h"
#include "../src/event_log.h"
#include "../src/util.h"
#include "../src/workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int run_agent_tests(void);

static int fails = 0;
#define EXPECT(c, m) do { \
    if (c) printf("ok  - %s\n", m); \
    else { printf("not ok - %s (%s:%d)\n", m, __FILE__, __LINE__); fails++; } \
} while (0)

static char *unique_root(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "/tmp/pinback-agent-%d-%lld",
             (int)getpid(), (long long)pin_monotonic_ms());
    char rm[128]; snprintf(rm, sizeof(rm), "rm -rf %s", buf);
    int rc = system(rm); (void)rc;
    return pin_xstrdup(buf);
}

static void wait_for_event(pin_event_log *log, const char *kind,
                           int timeout_ms, char *out_payload, size_t out_cap,
                           long long *out_seq) {
    long long start = (long long)pin_monotonic_ms();
    long long after = 0;
    if (out_payload) out_payload[0] = '\0';
    if (out_seq) *out_seq = -1;
    while ((long long)pin_monotonic_ms() - start < timeout_ms) {
        pin_event_log_status st;
        pin_event_log_status_get(log, &st);
        if (st.newest_seq > after) {
            /* Render snapshot, scan for the kind. */
            pin_buf snap; pin_buf_init(&snap);
            pin_event_log_render_snapshot(log, &snap);
            const char *p = snap.ptr;
            while (p && *p) {
                const char *kstart = strstr(p, "\"kind\":\"");
                if (!kstart) break;
                kstart += strlen("\"kind\":\"");
                const char *kend = strchr(kstart, '"');
                if (!kend) break;
                size_t klen = (size_t)(kend - kstart);
                if (klen == strlen(kind) && memcmp(kstart, kind, klen) == 0) {
                    /* Find the seq for this event by walking back. */
                    /* Just dump payload for the first match. */
                    const char *pl = strstr(kend, "\"payload\":");
                    if (pl && out_payload && out_cap > 0) {
                        pl += strlen("\"payload\":");
                        size_t i = 0;
                        int depth = 0;
                        while (*pl && i + 1 < out_cap) {
                            if (*pl == '{') depth++;
                            else if (*pl == '}') { if (--depth <= 0) { out_payload[i++] = *pl; break; } }
                            out_payload[i++] = *pl++;
                        }
                        out_payload[i] = '\0';
                    }
                    pin_buf_free(&snap);
                    return;
                }
                p = kend + 1;
            }
            pin_buf_free(&snap);
            after = st.newest_seq;
        }
        usleep(20 * 1000);
    }
}

static int log_event_count(pin_event_log *log, const char *kind) {
    pin_buf snap; pin_buf_init(&snap);
    pin_event_log_render_snapshot(log, &snap);
    int n = 0;
    const char *p = snap.ptr;
    char needle[64];
    snprintf(needle, sizeof(needle), "\"kind\":\"%s\"", kind);
    while (p && (p = strstr(p, needle)) != NULL) { n++; p += strlen(needle); }
    pin_buf_free(&snap);
    return n;
}

/* Turn-end (stderr +DWARFSTAR_WAITING) and prose (stdout) arrive on
 * separate streams, so poll until at least `min` events of `kind` exist. */
static int wait_for_count(pin_event_log *log, const char *kind, int min,
                          int timeout_ms) {
    long long start = (long long)pin_monotonic_ms();
    int n = 0;
    do {
        n = log_event_count(log, kind);
        if (n >= min) return n;
        usleep(20 * 1000);
    } while ((long long)pin_monotonic_ms() - start < timeout_ms);
    return n;
}

/* True if the rendered snapshot contains `needle`; polls up to timeout. */
static bool wait_snapshot_contains(pin_event_log *log, const char *needle,
                                   int timeout_ms) {
    long long start = (long long)pin_monotonic_ms();
    do {
        pin_buf snap; pin_buf_init(&snap);
        pin_event_log_render_snapshot(log, &snap);
        bool found = snap.ptr && strstr(snap.ptr, needle) != NULL;
        pin_buf_free(&snap);
        if (found) return true;
        usleep(20 * 1000);
    } while ((long long)pin_monotonic_ms() - start < timeout_ms);
    return false;
}

static const char *find_fake_agent(void) {
    /* tests run from repo root via Makefile so tools/fake-ds4-agent works. */
    static char buf[1024];
    if (access("tools/fake-ds4-agent", X_OK) == 0) {
        snprintf(buf, sizeof(buf), "%s/tools/fake-ds4-agent",
                 getcwd(NULL, 0)); /* leak ok in test */
        return buf;
    }
    if (access("./tools/fake-ds4-agent", X_OK) == 0) return "./tools/fake-ds4-agent";
    return "tools/fake-ds4-agent";
}

static void test_spawn_and_submit(void) {
    char *root = unique_root();
    pin_workspace_store *s = pin_workspace_store_open(root, 64);
    char ws_path[1024];
    snprintf(ws_path, sizeof(ws_path), "%s/wsA", root);
    char mk[2048]; snprintf(mk, sizeof(mk), "mkdir -p %s", ws_path);
    int rc = system(mk); (void)rc;
    char *id = NULL;
    pin_workspace_store_create(s, ws_path, NULL, &id, NULL);

    pin_agent_config cfg = {
        .agent_bin = find_fake_agent(),
        .spawn_ready_ms = 2000,
        .save_timeout_ms = 2000,
        .term_timeout_ms = 2000,
    };
    pin_agent *a = pin_agent_new(&cfg, s);
    EXPECT(a != NULL, "agent created");

    char *err = NULL;
    EXPECT(pin_agent_activate(a, id, &err), "activate workspace");
    if (err) { printf("activate err: %s\n", err); free(err); err = NULL; }

    pin_event_log *log = pin_workspace_store_event_log(s, id);

    EXPECT(pin_agent_submit(a, id, "hello", &err), "submit returns true");
    if (err) { printf("submit err: %s\n", err); free(err); err = NULL; }

    /* Wait for answer_end. */
    char payload[1024] = {0};
    long long seq = -1;
    wait_for_event(log, "answer_end", 3000, payload, sizeof(payload), &seq);

    EXPECT(wait_for_count(log, "user", 1, 2000) >= 1, "saw user event");
    EXPECT(wait_for_count(log, "answer", 1, 2000) >= 1, "saw answer event");
    /* The real agent renders tool actions as a wrench-prefixed line, which
     * the classifier turns into a tool_call event (no raw DSML, no separate
     * tool_result block). */
    EXPECT(wait_for_count(log, "tool_call", 1, 2000) >= 1, "saw tool_call event");
    EXPECT(wait_for_count(log, "answer_end", 1, 2000) >= 1, "saw answer_end");

    /* Submit on inactive workspace fails. */
    char *wid_other = NULL;
    char other[1024]; snprintf(other, sizeof(other), "%s/wsB", root);
    snprintf(mk, sizeof(mk), "mkdir -p %s", other);
    rc = system(mk); (void)rc;
    pin_workspace_store_create(s, other, NULL, &wid_other, NULL);
    err = NULL;
    EXPECT(!pin_agent_submit(a, wid_other, "hi", &err),
           "submit to inactive workspace rejected");
    if (err) free(err);

    pin_agent_free(a);
    pin_workspace_store_close(s);
    free(id); free(wid_other);
    char rm[2048]; snprintf(rm, sizeof(rm), "rm -rf %s", root);
    rc = system(rm); (void)rc;
    free(root);
}

static void test_switch_workspaces(void) {
    char *root = unique_root();
    pin_workspace_store *s = pin_workspace_store_open(root, 64);
    char wsA[1024]; snprintf(wsA, sizeof(wsA), "%s/A", root);
    char wsB[1024]; snprintf(wsB, sizeof(wsB), "%s/B", root);
    char mk[2048];
    snprintf(mk, sizeof(mk), "mkdir -p %s %s", wsA, wsB);
    int rc = system(mk); (void)rc;
    char *idA = NULL, *idB = NULL;
    pin_workspace_store_create(s, wsA, NULL, &idA, NULL);
    pin_workspace_store_create(s, wsB, NULL, &idB, NULL);

    pin_agent_config cfg = { .agent_bin = find_fake_agent(),
                             .spawn_ready_ms = 2000,
                             .save_timeout_ms = 3000,
                             .term_timeout_ms = 2000 };
    pin_agent *a = pin_agent_new(&cfg, s);
    pin_agent_activate(a, idA, NULL);
    pin_agent_submit(a, idA, "alpha-one in A", NULL);

    pin_event_log *logA = pin_workspace_store_event_log(s, idA);
    char payload[512];
    long long seq = -1;
    wait_for_event(logA, "answer_end", 3000, payload, sizeof(payload), &seq);

    /* Switch to B: A is torn down (no session save -- the agent can't),
     * a fresh agent spawns in B. */
    EXPECT(pin_agent_activate(a, idB, NULL), "activate B");

    pin_event_log *logB = pin_workspace_store_event_log(s, idB);
    pin_agent_submit(a, idB, "first turn in B", NULL);
    wait_for_event(logB, "answer_end", 3000, payload, sizeof(payload), &seq);
    EXPECT(log_event_count(logB, "user") >= 1, "B saw user event");

    /* Switch back to A: pinback stages A's prior transcript for re-prefill. */
    EXPECT(pin_agent_activate(a, idA, NULL), "activate A again");

    /* The next prompt in A should carry the prior conversation as context.
     * The fake echoes whatever prompt it receives, so A's log should now
     * contain the resume header and the earlier turn re-fed to the agent. */
    pin_event_log *logA2 = pin_workspace_store_event_log(s, idA);
    pin_agent_submit(a, idA, "alpha-two in A", NULL);
    /* "Resuming an earlier session" and the "User: <prior>" transcript line
     * only appear if the resume context was prepended to this prompt. */
    EXPECT(wait_snapshot_contains(logA2, "Resuming an earlier session", 3000),
           "resume context re-prefilled on switch-back");
    EXPECT(wait_snapshot_contains(logA2, "User: alpha-one in A", 3000),
           "prior turn carried into the resumed prompt");

    pin_agent_free(a);
    pin_workspace_store_close(s);
    free(idA); free(idB);
    char rm[2048]; snprintf(rm, sizeof(rm), "rm -rf %s", root);
    rc = system(rm); (void)rc;
    free(root);
}

static void test_reset(void) {
    char *root = unique_root();
    pin_workspace_store *s = pin_workspace_store_open(root, 64);
    char wsA[1024]; snprintf(wsA, sizeof(wsA), "%s/A", root);
    char mk[2048]; snprintf(mk, sizeof(mk), "mkdir -p %s", wsA);
    int rc = system(mk); (void)rc;
    char *idA = NULL;
    pin_workspace_store_create(s, wsA, NULL, &idA, NULL);

    pin_agent_config cfg = { .agent_bin = find_fake_agent(),
                             .spawn_ready_ms = 2000,
                             .save_timeout_ms = 3000,
                             .term_timeout_ms = 2000 };
    pin_agent *a = pin_agent_new(&cfg, s);
    pin_agent_activate(a, idA, NULL);
    pin_agent_submit(a, idA, "first", NULL);
    pin_event_log *log = pin_workspace_store_event_log(s, idA);
    char payload[512]; long long seq = -1;
    wait_for_event(log, "answer_end", 3000, payload, sizeof(payload), &seq);
    EXPECT(log_event_count(log, "user") == 1, "1 user event before reset");

    EXPECT(pin_agent_reset(a, idA, NULL), "reset succeeds");
    pin_event_log *log2 = pin_workspace_store_event_log(s, idA);
    pin_event_log_status st;
    pin_event_log_status_get(log2, &st);
    EXPECT(st.newest_seq <= 1, "events.log reset to small seq after reset");

    pin_workspace_meta got;
    pin_workspace_store_get(s, idA, &got);
    EXPECT(got.session_sha == NULL, "session_sha cleared on reset");
    pin_workspace_meta_free(&got);

    pin_agent_free(a);
    pin_workspace_store_close(s);
    free(idA);
    char rm[2048]; snprintf(rm, sizeof(rm), "rm -rf %s", root);
    rc = system(rm); (void)rc;
    free(root);
}

int run_agent_tests(void) {
    fails = 0;
    test_spawn_and_submit();
    test_switch_workspaces();
    test_reset();
    return fails ? 1 : 0;
}
