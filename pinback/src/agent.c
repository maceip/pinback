#include "agent.h"

#include "event_log.h"
#include "log.h"
#include "snapshot.h"
#include "util.h"
#include "workspace.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ds4-agent renders a tool action as a line beginning with the wrench
 * glyph U+1F6E0 U+FE0F (UTF-8: F0 9F 9B A0 EF B8 8F), e.g.
 * "<glyph> write  path=/tmp/x.txt". It does NOT emit raw DSML on stdout
 * in any mode -- verified against the real binary, see
 * docs/transport-findings.md. So the classifier keys tool activity off
 * this prefix rather than DSML block tags. Turn-end is the stderr
 * "+DWARFSTAR_WAITING" marker, not any stdout sentinel. */
#define TOOL_GLYPH "\xF0\x9F\x9B\xA0\xEF\xB8\x8F"

#define MAX_PROSE_FLUSH (4 * 1024)

struct pin_agent {
    pin_agent_config    cfg;
    pin_workspace_store *ws;

    pthread_mutex_t mu;             /* lifecycle: state + child pid */
    pthread_cond_t  cv;             /* signals state changes */
    pin_agent_state state;
    char            active_id[64];
    char            active_path[1024];

    pid_t           child_pid;
    int             child_stdin;     /* write end */
    int             child_stdout;    /* read end */
    int             child_stderr;    /* read end */
    pthread_t       reader;
    pthread_t       err_reader;
    bool            reader_running;
    bool            err_running;

    /* Resume context to prepend to the next prompt after a workspace
     * switch (transcript re-prefill). NULL when there is nothing to
     * resume. Owned by the agent; cleared once consumed by a submit. */
    char           *pending_resume;

    /* Counters. */
    long long       turns_total;
    long long       restarts_total;
    long long       last_spawn_ms;
};

const char *pin_agent_state_name(pin_agent_state s) {
    switch (s) {
        case PIN_AGENT_STATE_IDLE:     return "idle";
        case PIN_AGENT_STATE_SPAWNING: return "spawning";
        case PIN_AGENT_STATE_READY:    return "ready";
        case PIN_AGENT_STATE_BUSY:     return "busy";
        case PIN_AGENT_STATE_DRAINING: return "draining";
        case PIN_AGENT_STATE_DEAD:     return "dead";
    }
    return "unknown";
}

/* ====================================================================
 * Event emission helpers (always tagged with workspace_id).
 * ==================================================================== */

static void emit_event(pin_agent *a, const char *kind,
                       const char *json, size_t len) {
    pin_event_log *log = pin_workspace_store_event_log(a->ws, a->active_id);
    if (!log) return;
    pin_event_log_append(log, kind, json, len);
}

static void emit_state(pin_agent *a) {
    pin_buf b;
    pin_buf_init(&b);
    pin_buf_putc(&b, '{');
    pin_buf_puts(&b, "\"state\":");
    pin_json_str(&b, pin_agent_state_name(a->state));
    pin_buf_puts(&b, ",\"workspace\":");
    if (a->active_id[0]) {
        pin_buf_putc(&b, '{');
        pin_buf_puts(&b, "\"id\":");
        pin_json_str(&b, a->active_id);
        pin_buf_puts(&b, ",\"path\":");
        pin_json_str(&b, a->active_path);
        pin_buf_putc(&b, '}');
    } else {
        pin_buf_puts(&b, "null");
    }
    pin_buf_putc(&b, '}');
    if (a->active_id[0]) {
        emit_event(a, "agent.state", b.ptr, b.len);
    }
    pin_buf_free(&b);
}

static void emit_user(pin_agent *a, const char *text) {
    pin_buf b;
    pin_buf_init(&b);
    pin_buf_putc(&b, '{');
    pin_buf_puts(&b, "\"text\":");
    pin_json_str(&b, text ? text : "");
    pin_buf_putc(&b, '}');
    emit_event(a, "user", b.ptr, b.len);
    pin_buf_free(&b);
}

static void emit_prose(pin_agent *a, const char *text, size_t len) {
    if (!len) return;
    pin_buf cleaned;
    pin_buf_init(&cleaned);
    /* The classifier already stripped ANSI, but we still sanitize for
     * untrusted multi-byte fragments. */
    pin_text_sanitize(&cleaned, text, len, MAX_PROSE_FLUSH);
    if (cleaned.len == 0) { pin_buf_free(&cleaned); return; }
    pin_buf b;
    pin_buf_init(&b);
    pin_buf_putc(&b, '{');
    pin_buf_puts(&b, "\"text\":");
    pin_json_strn(&b, cleaned.ptr, cleaned.len);
    pin_buf_putc(&b, '}');
    emit_event(a, "answer", b.ptr, b.len);
    pin_buf_free(&b);
    pin_buf_free(&cleaned);
}

static void emit_dsml_tool_call(pin_agent *a, const char *raw, size_t len) {
    pin_buf b;
    pin_buf_init(&b);
    pin_buf_putc(&b, '{');
    pin_buf_puts(&b, "\"raw\":");
    pin_json_strn(&b, raw, len);
    pin_buf_putc(&b, '}');
    emit_event(a, "tool_call", b.ptr, b.len);
    pin_buf_free(&b);
}

static void emit_turn_end(pin_agent *a) {
    emit_event(a, "answer_end", "{}", 2);
}

/* Shadow-git dir for the active workspace's per-turn change tracking. */
static bool agent_snapshot_git_dir(pin_agent *a, char *buf, size_t cap) {
    if (!a->active_id[0]) return false;
    char wsd[1024];
    if (!pin_workspace_store_ws_dir(a->ws, a->active_id, wsd, sizeof(wsd)))
        return false;
    int n = snprintf(buf, cap, "%s/snapshot.git", wsd);
    return n > 0 && (size_t)n < cap;
}

static void emit_turn_diff(pin_agent *a, const char *diff, size_t len,
                           int nfiles) {
    pin_buf b;
    pin_buf_init(&b);
    pin_buf_putc(&b, '{');
    pin_buf_puts(&b, "\"files\":");
    pin_buf_printf(&b, "%d", nfiles);
    pin_buf_puts(&b, ",\"diff\":");
    pin_json_strn(&b, diff, len);
    pin_buf_putc(&b, '}');
    emit_event(a, "turn_diff", b.ptr, b.len);
    pin_buf_free(&b);
}

/* At turn end, diff the workspace against the start-of-turn snapshot and
 * publish a turn_diff event (agent-independent change review). */
static void agent_emit_turn_changes(pin_agent *a) {
    char gd[1100];
    if (!a->active_path[0] || !agent_snapshot_git_dir(a, gd, sizeof(gd))) return;
    pin_buf d;
    pin_buf_init(&d);
    if (pin_snapshot_diff(gd, a->active_path, &d) && d.len > 0) {
        int nf = 0;
        const char *p = d.ptr;
        while ((p = strstr(p, "diff --git ")) != NULL) { nf++; p += 11; }
        emit_turn_diff(a, d.ptr, d.len, nf);
    }
    pin_buf_free(&d);
}

static void emit_error(pin_agent *a, const char *kind, const char *msg) {
    pin_buf b;
    pin_buf_init(&b);
    pin_buf_putc(&b, '{');
    pin_buf_puts(&b, "\"kind\":");
    pin_json_str(&b, kind ? kind : "agent");
    pin_buf_puts(&b, ",\"message\":");
    pin_json_str(&b, msg ? msg : "");
    pin_buf_putc(&b, '}');
    emit_event(a, "error", b.ptr, b.len);
    pin_buf_free(&b);
}

/* ====================================================================
 * Classifier
 * ====================================================================
 *
 * The real ds4-agent (non-interactive) writes clean UTF-8 to stdout:
 * prose plus tool-activity lines that begin with TOOL_GLYPH. There is
 * no raw DSML and no stdout turn-end sentinel -- turn-end is the stderr
 * +DWARFSTAR_WAITING marker. So the classifier is line-oriented:
 *   - a line starting with TOOL_GLYPH -> tool_call event
 *   - any other line                  -> answer (prose) event
 * Bytes buffer until a newline; a high-water mark flushes an over-long
 * unterminated line so the UI never stalls. */

typedef struct {
    pin_buf buf;          /* clean (ANSI-stripped) bytes */
    bool    in_ansi;      /* mid-CSI parse */
} classifier;

static void classifier_init(classifier *c) {
    pin_buf_init(&c->buf);
    c->in_ansi = false;
}

static void classifier_free(classifier *c) {
    pin_buf_free(&c->buf);
}

/* Strip ANSI CSI/OSC sequences. Drop ESC + [..letter]. Drop bare ESC. */
static void classifier_feed(classifier *c, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char)p[i];
        if (c->in_ansi) {
            /* CSI sequence ends at first byte in 0x40..0x7E. */
            if (b >= 0x40 && b <= 0x7E) c->in_ansi = false;
            continue;
        }
        if (b == 0x1B) {                  /* ESC */
            c->in_ansi = true;
            continue;
        }
        if (b == '\r') continue;          /* drop carriage returns */
        pin_buf_putc(&c->buf, (char)b);
    }
}

/* Mark the supervisor as READY and emit answer_end.
 *
 * Deliberately lock-free: the supervisor's state field is written from
 * either the activate thread (under a->mu) or this reader thread. Taking
 * a->mu here could deadlock with an activate in progress; the state
 * field is read via pin_agent_status_get which only races on a 1-byte
 * enum store -- a stale READY/BUSY observation is acceptable for v0. */
static void classifier_signal_turn_end(pin_agent *a) {
    /* Only fire answer_end when we're actually mid-turn. ds4-agent's
     * +DWARFSTAR_WAITING marker is also emitted at initial readiness,
     * which would otherwise insert spurious answer_end events into the
     * log before the user has even spoken. */
    if (a->state != PIN_AGENT_STATE_BUSY) return;
    a->state = PIN_AGENT_STATE_READY;
    emit_turn_end(a);
    agent_emit_turn_changes(a);
}

/* Classify and emit one logical line (newline already stripped). A line
 * that begins with the wrench glyph is a tool action; everything else is
 * prose. `had_newline` is true for a complete line (we re-attach the
 * newline so multi-line answers keep their shape) and false for a tail
 * flushed without its terminator. */
static void classifier_emit_line(pin_agent *a, const char *s, size_t n,
                                 bool had_newline) {
    static const char glyph[] = TOOL_GLYPH;
    const size_t glen = sizeof(glyph) - 1;
    if (n >= glen && memcmp(s, glyph, glen) == 0) {
        const char *p = s + glen;
        size_t pn = n - glen;
        while (pn && *p == ' ') { p++; pn--; }
        emit_dsml_tool_call(a, p, pn);   /* emits a "tool_call" event */
        return;
    }
    if (had_newline) {
        pin_buf b;
        pin_buf_init(&b);
        pin_buf_append(&b, s, n);
        pin_buf_putc(&b, '\n');
        emit_prose(a, b.ptr, b.len);
        pin_buf_free(&b);
    } else {
        emit_prose(a, s, n);
    }
}

/* Emit complete lines; hold the partial tail for the next feed. A
 * high-water mark (or flush_all when the stream closes) emits an
 * over-long unterminated tail so the UI never stalls. */
static void classifier_drain(classifier *c, pin_agent *a, bool flush_all) {
    size_t start = 0;
    for (;;) {
        if (start >= c->buf.len) break;
        char *nl = memchr(c->buf.ptr + start, '\n', c->buf.len - start);
        if (!nl) break;
        size_t llen = (size_t)(nl - (c->buf.ptr + start));
        classifier_emit_line(a, c->buf.ptr + start, llen, true);
        start = (size_t)(nl - c->buf.ptr) + 1;
    }
    if (start > 0) {
        memmove(c->buf.ptr, c->buf.ptr + start, c->buf.len - start);
        c->buf.len -= start;
        c->buf.ptr[c->buf.len] = '\0';
    }
    if (c->buf.len > 0 && (flush_all || c->buf.len > MAX_PROSE_FLUSH)) {
        classifier_emit_line(a, c->buf.ptr, c->buf.len, false);
        c->buf.len = 0;
        c->buf.ptr[0] = '\0';
    }
}

/* ====================================================================
 * Reader threads
 * ==================================================================== */

typedef struct {
    pin_agent *a;
} reader_args;

static void *stdout_reader(void *arg) {
    reader_args *ra = arg;
    pin_agent *a = ra->a;
    free(ra);

    classifier c;
    classifier_init(&c);
    char buf[4096];
    while (1) {
        ssize_t n = read(a->child_stdout, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        classifier_feed(&c, buf, (size_t)n);
        classifier_drain(&c, a, false);
    }
    classifier_drain(&c, a, true);
    classifier_free(&c);

    pthread_mutex_lock(&a->mu);
    a->reader_running = false;
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);
    return NULL;
}

/* Real ds4-agent's non-interactive mode publishes idle/queue markers on
 * stderr. "+DWARFSTAR_WAITING" means "model is idle, awaiting input" --
 * i.e. either initial readiness or the end of a turn. We treat it as a
 * turn-end signal so the supervisor can flip BUSY -> READY. */
static bool stderr_line_is_idle_marker(const char *s, size_t n) {
    static const char k[] = "+DWARFSTAR_WAITING";
    if (n < sizeof(k) - 1) return false;
    return memcmp(s, k, sizeof(k) - 1) == 0;
}

static void *stderr_reader(void *arg) {
    reader_args *ra = arg;
    pin_agent *a = ra->a;
    free(ra);
    pin_buf line;
    pin_buf_init(&line);
    char buf[2048];
    while (1) {
        ssize_t n = read(a->child_stderr, buf, sizeof(buf));
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                if (line.len) {
                    PIN_LOG_DEBUGF("agent.stderr", "%.*s",
                                   (int)line.len, line.ptr);
                    if (stderr_line_is_idle_marker(line.ptr, line.len)) {
                        classifier_signal_turn_end(a);
                    }
                    pin_buf_clear(&line);
                }
            } else if (buf[i] != '\r') {
                pin_buf_putc(&line, buf[i]);
                if (line.len > 4096) pin_buf_clear(&line);
            }
        }
    }
    if (line.len) {
        PIN_LOG_DEBUGF("agent.stderr", "%.*s", (int)line.len, line.ptr);
    }
    pin_buf_free(&line);
    pthread_mutex_lock(&a->mu);
    a->err_running = false;
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);
    return NULL;
}

/* ====================================================================
 * Spawn / kill
 * ==================================================================== */

static bool spawn_child_locked(pin_agent *a, const char *path) {
    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        if (in_pipe[0] >= 0)  close(in_pipe[0]);
        if (in_pipe[1] >= 0)  close(in_pipe[1]);
        if (out_pipe[0] >= 0) close(out_pipe[0]);
        if (out_pipe[1] >= 0) close(out_pipe[1]);
        if (err_pipe[0] >= 0) close(err_pipe[0]);
        if (err_pipe[1] >= 0) close(err_pipe[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return false;
    }
    if (pid == 0) {
        /* child */
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[1]); close(out_pipe[0]); close(err_pipe[0]);
        close(in_pipe[0]); close(out_pipe[1]); close(err_pipe[1]);
        /* New process group so SIGTERM hits the whole tree. */
        setpgid(0, 0);
        /* ds4-agent's --chdir flips wd before loading runtime assets, so
         * the relative "metal/<x>.metal" lookup fails for non-~/ds4 cwd.
         * Resolve the agent binary's realpath and pre-export absolute
         * DS4_METAL_*_SOURCE env vars from <bin_dir>/metal. ds4-agent
         * honors the env override before falling back to the relative
         * path, so this lets the agent boot from any --chdir target. */
        if (a->cfg.agent_bin && *a->cfg.agent_bin) {
            char real[4096];
            if (realpath(a->cfg.agent_bin, real)) {
                char *slash = strrchr(real, '/');
                if (slash) {
                    *slash = '\0';
                    static const struct { const char *env, *file; } metal_map[] = {
                        {"DS4_METAL_FLASH_ATTN_SOURCE", "flash_attn.metal"},
                        {"DS4_METAL_DENSE_SOURCE",      "dense.metal"},
                        {"DS4_METAL_MOE_SOURCE",        "moe.metal"},
                        {"DS4_METAL_DSV4_HC_SOURCE",    "dsv4_hc.metal"},
                        {"DS4_METAL_UNARY_SOURCE",      "unary.metal"},
                        {"DS4_METAL_DSV4_KV_SOURCE",    "dsv4_kv.metal"},
                        {"DS4_METAL_DSV4_ROPE_SOURCE",  "dsv4_rope.metal"},
                        {"DS4_METAL_DSV4_MISC_SOURCE",  "dsv4_misc.metal"},
                        {"DS4_METAL_ARGSORT_SOURCE",    "argsort.metal"},
                        {"DS4_METAL_CPY_SOURCE",        "cpy.metal"},
                        {"DS4_METAL_CONCAT_SOURCE",     "concat.metal"},
                        {"DS4_METAL_GET_ROWS_SOURCE",   "get_rows.metal"},
                        {"DS4_METAL_SUM_ROWS_SOURCE",   "sum_rows.metal"},
                        {"DS4_METAL_SOFTMAX_SOURCE",    "softmax.metal"},
                        {"DS4_METAL_REPEAT_SOURCE",     "repeat.metal"},
                        {"DS4_METAL_GLU_SOURCE",        "glu.metal"},
                        {"DS4_METAL_NORM_SOURCE",       "norm.metal"},
                        {"DS4_METAL_BIN_SOURCE",        "bin.metal"},
                        {"DS4_METAL_SET_ROWS_SOURCE",   "set_rows.metal"},
                    };
                    char buf[4200];
                    for (size_t i = 0; i < sizeof(metal_map)/sizeof(metal_map[0]); i++) {
                        if (getenv(metal_map[i].env)) continue;
                        snprintf(buf, sizeof(buf), "%s/metal/%s",
                                 real, metal_map[i].file);
                        setenv(metal_map[i].env, buf, 0);
                    }
                }
            }
        }
        char *argv[16];
        int ai = 0;
        argv[ai++] = (char *)a->cfg.agent_bin;
        argv[ai++] = (char *)"--non-interactive";
        argv[ai++] = (char *)"--chdir";
        argv[ai++] = (char *)path;
        if (a->cfg.model_path && *a->cfg.model_path) {
            argv[ai++] = (char *)"--model";
            argv[ai++] = (char *)a->cfg.model_path;
        }
        argv[ai] = NULL;
        execvp(argv[0], argv);
        /* If exec fails, write to stderr_pipe so parent logs it. */
        dprintf(STDERR_FILENO, "execvp %s failed: errno=%d\n", argv[0], errno);
        _exit(127);
    }
    /* parent */
    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    a->child_pid     = pid;
    a->child_stdin   = in_pipe[1];
    a->child_stdout  = out_pipe[0];
    a->child_stderr  = err_pipe[0];
    a->last_spawn_ms = (long long)pin_monotonic_ms();

    reader_args *ra1 = pin_xcalloc(1, sizeof(*ra1)); ra1->a = a;
    reader_args *ra2 = pin_xcalloc(1, sizeof(*ra2)); ra2->a = a;
    a->reader_running = true;
    a->err_running    = true;
    if (pthread_create(&a->reader, NULL, stdout_reader, ra1) != 0) {
        a->reader_running = false;
        free(ra1);
    }
    if (pthread_create(&a->err_reader, NULL, stderr_reader, ra2) != 0) {
        a->err_running = false;
        free(ra2);
    }
    pthread_detach(a->reader);
    pthread_detach(a->err_reader);

    PIN_LOG_INFOF("agent.spawn", "pid=%d path=%s", (int)pid, path);
    return true;
}

static void close_child_io(pin_agent *a) {
    if (a->child_stdin  >= 0) { close(a->child_stdin);  a->child_stdin  = -1; }
    if (a->child_stdout >= 0) { close(a->child_stdout); a->child_stdout = -1; }
    if (a->child_stderr >= 0) { close(a->child_stderr); a->child_stderr = -1; }
}

/* Caller MUST hold a->mu. Drops the mutex while waiting on cv so the
 * reader threads can lock it to set their running=false flags. */
static void wait_reader_threads(pin_agent *a) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;
    while ((a->reader_running || a->err_running)) {
        if (pthread_cond_timedwait(&a->cv, &a->mu, &ts) != 0) break;
    }
}

static bool kill_child_locked(pin_agent *a, int term_timeout_ms) {
    if (a->child_pid <= 0) return true;
    pid_t pgid = -a->child_pid;
    kill(pgid, SIGTERM);
    /* Close stdin so the agent's read loop exits naturally. */
    if (a->child_stdin >= 0) { close(a->child_stdin); a->child_stdin = -1; }
    long long deadline = (long long)pin_monotonic_ms() + term_timeout_ms;
    while ((long long)pin_monotonic_ms() < deadline) {
        int st;
        pid_t r = waitpid(a->child_pid, &st, WNOHANG);
        if (r == a->child_pid) {
            a->child_pid = -1;
            close_child_io(a);
            wait_reader_threads(a);
            return true;
        }
        usleep(50 * 1000);
    }
    kill(pgid, SIGKILL);
    int st;
    waitpid(a->child_pid, &st, 0);
    a->child_pid = -1;
    close_child_io(a);
    wait_reader_threads(a);
    return true;
}

/* ====================================================================
 * stdin write
 * ==================================================================== */

static bool stdin_write_line(pin_agent *a, const char *line) {
    if (a->child_stdin < 0) return false;
    size_t llen = strlen(line);
    const char *p = line;
    size_t left = llen;
    while (left > 0) {
        ssize_t w = write(a->child_stdin, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w; left -= (size_t)w;
    }
    if (write(a->child_stdin, "\n", 1) < 0) return false;
    return true;
}

/* ====================================================================
 * Resume (transcript re-prefill)
 * ====================================================================
 *
 * ds4-agent cannot save/restore a session over the non-interactive pipe
 * (slash commands are TUI-only, verified -- see docs/transport-findings.md),
 * so pinback owns continuity. On switching to a workspace that already
 * has conversation history, we render that history to text and stash it
 * in a->pending_resume; the next submit prepends it (invisibly -- the
 * user event still shows only what the user typed) so the fresh agent
 * picks up the prior context. Files on disk and the UI's own event log
 * already carry the rest. */

#define RESUME_MAX_BYTES (16 * 1024)

static void build_pending_resume(pin_agent *a, const char *workspace_id) {
    free(a->pending_resume);
    a->pending_resume = NULL;
    pin_event_log *log = pin_workspace_store_event_log(a->ws, workspace_id);
    if (!log) return;
    pin_buf t;
    pin_buf_init(&t);
    pin_event_log_render_transcript(log, &t, RESUME_MAX_BYTES);
    if (t.len == 0) { pin_buf_free(&t); return; }
    pin_buf b;
    pin_buf_init(&b);
    pin_buf_puts(&b,
        "[Resuming an earlier session in this workspace. Our conversation so "
        "far is below. Continue from here; do not repeat it back.]\n\n");
    pin_buf_append(&b, t.ptr, t.len);
    pin_buf_puts(&b, "\n[End of earlier conversation. Continue below.]\n");
    pin_buf_free(&t);
    a->pending_resume = pin_buf_detach(&b);
}

/* ====================================================================
 * Lifecycle: activate / submit / reset / abort
 * ==================================================================== */

pin_agent *pin_agent_new(const pin_agent_config *cfg, pin_workspace_store *ws) {
    pin_agent *a = pin_xcalloc(1, sizeof(*a));
    a->cfg = *cfg;
    if (a->cfg.spawn_ready_ms <= 0) a->cfg.spawn_ready_ms = 5000;
    if (a->cfg.term_timeout_ms <= 0) a->cfg.term_timeout_ms = 3000;
    a->ws = ws;
    a->state = PIN_AGENT_STATE_IDLE;
    a->child_pid = -1;
    a->child_stdin = a->child_stdout = a->child_stderr = -1;
    pthread_mutex_init(&a->mu, NULL);
    pthread_cond_init(&a->cv, NULL);
    return a;
}

void pin_agent_free(pin_agent *a) {
    if (!a) return;
    pthread_mutex_lock(&a->mu);
    if (a->child_pid > 0) kill_child_locked(a, a->cfg.term_timeout_ms);
    pthread_mutex_unlock(&a->mu);
    pthread_mutex_destroy(&a->mu);
    pthread_cond_destroy(&a->cv);
    free(a->pending_resume);
    free(a);
}

bool pin_agent_activate(pin_agent *a, const char *workspace_id, char **out_err) {
    if (out_err) *out_err = NULL;
    if (!a || !workspace_id || !*workspace_id) {
        if (out_err) *out_err = pin_xstrdup("workspace id required");
        return false;
    }
    pin_workspace_meta meta;
    if (!pin_workspace_store_get(a->ws, workspace_id, &meta)) {
        if (out_err) *out_err = pin_xstrdup("unknown workspace id");
        return false;
    }

    pthread_mutex_lock(&a->mu);
    /* If already on this workspace and child is alive, just return ok. */
    if (a->child_pid > 0 && strcmp(a->active_id, workspace_id) == 0) {
        pthread_mutex_unlock(&a->mu);
        pin_workspace_meta_free(&meta);
        return true;
    }
    /* A child for a different workspace just gets torn down. ds4-agent
     * cannot persist its session over the pipe, so there is nothing to
     * save here; the workspace's event log already holds the transcript
     * and the files are on disk. */
    if (a->child_pid > 0) {
        a->state = PIN_AGENT_STATE_DRAINING;
        kill_child_locked(a, a->cfg.term_timeout_ms);
    }
    /* Bind to new workspace. */
    snprintf(a->active_id, sizeof(a->active_id), "%s", meta.id);
    snprintf(a->active_path, sizeof(a->active_path), "%s", meta.path);
    pin_workspace_store_set_active(a->ws, meta.id);
    a->state = PIN_AGENT_STATE_SPAWNING;
    bool ok = spawn_child_locked(a, meta.path);
    if (!ok) {
        a->state = PIN_AGENT_STATE_DEAD;
        a->restarts_total++;
        pthread_mutex_unlock(&a->mu);
        if (out_err) *out_err = pin_xstrdup("failed to spawn agent");
        pin_workspace_meta_free(&meta);
        return false;
    }
    /* Resume continuity: if this workspace already has a conversation,
     * stage its transcript to prepend to the next prompt. */
    build_pending_resume(a, meta.id);
    a->state = PIN_AGENT_STATE_READY;
    emit_state(a);
    pthread_mutex_unlock(&a->mu);
    pin_workspace_meta_free(&meta);
    return true;
}

bool pin_agent_submit(pin_agent *a, const char *workspace_id,
                      const char *user_text, char **out_err) {
    if (out_err) *out_err = NULL;
    if (!a || !user_text || !*user_text) {
        if (out_err) *out_err = pin_xstrdup("text required");
        return false;
    }
    pthread_mutex_lock(&a->mu);
    if (workspace_id && a->active_id[0] &&
        strcmp(workspace_id, a->active_id) != 0) {
        pthread_mutex_unlock(&a->mu);
        if (out_err) *out_err = pin_xstrdup("workspace is not active");
        return false;
    }
    if (a->child_pid <= 0) {
        pthread_mutex_unlock(&a->mu);
        if (out_err) *out_err = pin_xstrdup("agent not running");
        return false;
    }
    if (a->state == PIN_AGENT_STATE_BUSY) {
        pthread_mutex_unlock(&a->mu);
        if (out_err) *out_err = pin_xstrdup("agent is busy");
        return false;
    }
    a->state = PIN_AGENT_STATE_BUSY;
    a->turns_total++;
    /* Snapshot the workspace so we can diff what this turn changes. */
    {
        char gd[1100];
        if (a->active_path[0] && agent_snapshot_git_dir(a, gd, sizeof(gd)))
            pin_snapshot_begin(gd, a->active_path);
    }
    /* Emit the user event with exactly what the user typed. */
    emit_user(a, user_text);
    /* On the first prompt after a workspace switch, silently prepend the
     * staged transcript so the fresh agent has the prior context. */
    char *combined = NULL;
    const char *to_send = user_text;
    if (a->pending_resume) {
        pin_buf b;
        pin_buf_init(&b);
        pin_buf_puts(&b, a->pending_resume);
        pin_buf_puts(&b, "\n\n");
        pin_buf_puts(&b, user_text);
        combined = pin_buf_detach(&b);
        to_send = combined;
        free(a->pending_resume);
        a->pending_resume = NULL;
    }
    bool ok = stdin_write_line(a, to_send);
    free(combined);
    if (!ok) {
        a->state = PIN_AGENT_STATE_DEAD;
        emit_error(a, "agent.stdin", "write to agent stdin failed");
        pthread_mutex_unlock(&a->mu);
        if (out_err) *out_err = pin_xstrdup("agent stdin write failed");
        return false;
    }
    /* The reader thread will emit answer + answer_end via the
     * classifier; we don't synthesize answer_end here. The fake/real
     * agent emits a sentinel line "<<<TURN_END>>>" or equivalent which
     * we recognize below. (For v0 we emit answer_end on classifier
     * turn-end heuristic in stdout_reader's drain.) */
    pthread_mutex_unlock(&a->mu);
    return true;
}

bool pin_agent_reset(pin_agent *a, const char *workspace_id, char **out_err) {
    if (out_err) *out_err = NULL;
    if (!a || !workspace_id) {
        if (out_err) *out_err = pin_xstrdup("workspace id required");
        return false;
    }
    pthread_mutex_lock(&a->mu);
    bool was_active = (a->child_pid > 0 && strcmp(a->active_id, workspace_id) == 0);
    if (was_active) {
        a->state = PIN_AGENT_STATE_DRAINING;
        kill_child_locked(a, a->cfg.term_timeout_ms);
    }
    pthread_mutex_unlock(&a->mu);
    char *err = NULL;
    bool ok = pin_workspace_store_reset(a->ws, workspace_id, &err);
    if (!ok) {
        if (out_err) *out_err = err;
        else free(err);
        return false;
    }
    if (was_active) {
        return pin_agent_activate(a, workspace_id, out_err);
    }
    return true;
}

void pin_agent_abort(pin_agent *a) {
    if (!a) return;
    pthread_mutex_lock(&a->mu);
    if (a->child_pid > 0) {
        kill(a->child_pid, SIGINT);
        emit_event(a, "agent.aborted", "{}", 2);
        a->state = PIN_AGENT_STATE_READY;
    }
    pthread_mutex_unlock(&a->mu);
}

void pin_agent_status_get(pin_agent *a, pin_agent_status *out) {
    memset(out, 0, sizeof(*out));
    if (!a) return;
    pthread_mutex_lock(&a->mu);
    out->state = a->state;
    snprintf(out->active_workspace_id,   sizeof(out->active_workspace_id),
             "%s", a->active_id);
    snprintf(out->active_workspace_path, sizeof(out->active_workspace_path),
             "%s", a->active_path);
    out->child_pid       = a->child_pid;
    out->last_spawn_ms   = a->last_spawn_ms;
    out->turns_total     = a->turns_total;
    out->restarts_total  = a->restarts_total;
    pthread_mutex_unlock(&a->mu);
}
