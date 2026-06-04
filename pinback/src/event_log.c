#include "event_log.h"

#include "http.h"
#include "log.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

/* ====================================================================
 * Internal types
 * ====================================================================
 *
 * The ring buffer holds the most recent `cap` events. Each slot owns
 * a malloc'd, JSON-encoded payload (already wrapped as a complete
 * event object: {"seq":..., "kind":..., "payload":..., ...}). When a
 * slot is overwritten, the old payload is freed.
 *
 * Subscribers register on the live log. A subscriber is parked in the
 * caller's thread (pin_event_log_serve_subscriber). It owns its fd.
 * Broadcast is push: the producer thread iterates the subscriber list
 * under a read-lock and writes the formatted event to each fd. A
 * subscriber whose write fails (slow or gone) is marked dead; the
 * producer reaps them lazily.
 *
 * The on-disk file is append-only. Each line is one JSON event object
 * matching the in-memory payload. We fsync-batch every 16 events to
 * avoid latency spikes; on close we always fsync. */

typedef struct {
    long long  seq;
    char      *json;       /* full event object including trailing nothing */
    size_t     json_len;
} ring_slot;

struct pin_subscriber {
    pin_sub_kind  kind;
    int           fd;
    pthread_mutex_t fd_mu;     /* serialize writes to this fd */
    bool          dead;
    struct pin_subscriber *next;
    struct pin_subscriber *prev;
};

struct pin_event_log {
    pthread_rwlock_t rw;       /* protects ring + cursors + subscribers */

    /* Ring */
    ring_slot   *ring;
    size_t       cap;
    size_t       used;
    size_t       head;          /* index of next slot to write */
    long long    newest_seq;
    long long    oldest_seq;
    long long    generation;

    /* Subscribers (doubly linked list) */
    struct pin_subscriber *subs_head;
    struct pin_subscriber *subs_tail;
    size_t subs_count;

    /* On-disk */
    int          fd;
    size_t       writes_since_fsync;

    /* Lifecycle */
    bool         closing;
};

/* ====================================================================
 * Helpers
 * ==================================================================== */

static void slot_free(ring_slot *s) {
    free(s->json);
    s->json = NULL;
    s->json_len = 0;
    s->seq = 0;
}

static void log_writeln_locked(pin_event_log *log, const char *json, size_t len) {
    if (log->fd < 0) return;
    struct iovec iov[2] = {
        {.iov_base = (void *)json, .iov_len = len},
        {.iov_base = (void *)"\n", .iov_len = 1},
    };
    size_t total = len + 1;
    while (total > 0) {
        ssize_t w = writev(log->fd, iov, 2);
        if (w < 0) {
            if (errno == EINTR) continue;
            PIN_LOG_WARNF("event_log.write_fail", "errno=%d", errno);
            return;
        }
        if ((size_t)w >= total) break;
        if ((size_t)w < iov[0].iov_len) {
            iov[0].iov_base = (char *)iov[0].iov_base + w;
            iov[0].iov_len  -= (size_t)w;
        } else {
            size_t rest = (size_t)w - iov[0].iov_len;
            iov[0] = iov[1];
            iov[0].iov_base = (char *)iov[0].iov_base + rest;
            iov[0].iov_len  -= rest;
            iov[1].iov_len   = 0;
        }
        total -= (size_t)w;
    }
    log->writes_since_fsync++;
    if (log->writes_since_fsync >= 16) {
        fsync(log->fd);
        log->writes_since_fsync = 0;
    }
}

/* Serialize one event into a complete JSON object. Caller frees *out. */
static void format_event_json(long long seq, long long generation,
                              const char *kind,
                              const char *raw_payload, size_t raw_payload_len,
                              char **out, size_t *out_len) {
    pin_buf b;
    pin_buf_init(&b);
    char ts[32];
    pin_iso8601_ms(ts, sizeof(ts));
    pin_buf_putc(&b, '{');
    pin_buf_puts(&b, "\"seq\":");
    pin_buf_printf(&b, "%lld", seq);
    pin_buf_puts(&b, ",\"generation\":");
    pin_buf_printf(&b, "%lld", generation);
    pin_buf_puts(&b, ",\"ts\":");
    pin_json_str(&b, ts);
    pin_buf_puts(&b, ",\"kind\":");
    pin_json_str(&b, kind ? kind : "unknown");
    pin_buf_puts(&b, ",\"payload\":");
    /* raw_payload is already a JSON value (object / string / etc.) */
    if (raw_payload && raw_payload_len) {
        pin_buf_append(&b, raw_payload, raw_payload_len);
    } else {
        pin_buf_puts(&b, "null");
    }
    pin_buf_putc(&b, '}');
    *out_len = b.len;
    *out = pin_buf_detach(&b);
}

/* ====================================================================
 * Subscriber list
 * ==================================================================== */

static pin_subscriber *subs_register(pin_event_log *log, int fd, pin_sub_kind kind) {
    pin_subscriber *s = pin_xcalloc(1, sizeof(*s));
    s->kind = kind;
    s->fd   = fd;
    pthread_mutex_init(&s->fd_mu, NULL);
    pthread_rwlock_wrlock(&log->rw);
    s->prev = log->subs_tail;
    s->next = NULL;
    if (log->subs_tail) log->subs_tail->next = s;
    else                log->subs_head      = s;
    log->subs_tail = s;
    log->subs_count++;
    pthread_rwlock_unlock(&log->rw);
    return s;
}

static void subs_unregister_unlocked(pin_event_log *log, pin_subscriber *s) {
    if (s->prev) s->prev->next = s->next; else log->subs_head = s->next;
    if (s->next) s->next->prev = s->prev; else log->subs_tail = s->prev;
    log->subs_count--;
}

static void subs_close(pin_subscriber *s) {
    if (s->fd >= 0) {
        if (s->kind == PIN_SUB_KIND_WS) {
            pin_ws_send_close(s->fd, 1000);
        }
        close(s->fd);
        s->fd = -1;
    }
    pthread_mutex_destroy(&s->fd_mu);
    free(s);
}

static bool sub_send_event(pin_subscriber *s, long long seq, const char *json, size_t json_len) {
    pthread_mutex_lock(&s->fd_mu);
    bool ok;
    if (s->kind == PIN_SUB_KIND_SSE) {
        ok = pin_http_sse_emit(s->fd, seq, "event", json, json_len);
    } else {
        ok = pin_ws_send_text(s->fd, json, json_len);
    }
    if (!ok) s->dead = true;
    pthread_mutex_unlock(&s->fd_mu);
    return ok;
}

/* ====================================================================
 * Open / close
 * ==================================================================== */

pin_event_log *pin_event_log_open(const char *path, size_t ring_capacity) {
    if (ring_capacity == 0) ring_capacity = 1024;
    pin_event_log *log = pin_xcalloc(1, sizeof(*log));
    pthread_rwlock_init(&log->rw, NULL);
    log->cap = ring_capacity;
    log->ring = pin_xcalloc(log->cap, sizeof(ring_slot));
    log->fd = -1;
    log->generation = 1;
    log->newest_seq = 0;
    log->oldest_seq = 1;  /* "no events yet" sentinel: oldest > newest */

    if (path) {
        log->fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0640);
        if (log->fd < 0) {
            PIN_LOG_WARNF("event_log.open_fail", "path=%s errno=%d", path, errno);
        } else {
            /* Replay tail. We read up to 256 KiB from the end of the file
             * and parse line-by-line, restoring at most ring_capacity
             * events. We also extract the highest seq and generation
             * seen, then increment generation by 1 for this run. */
            off_t fsize = lseek(log->fd, 0, SEEK_END);
            if (fsize > 0) {
                size_t replay = (fsize > 262144) ? 262144 : (size_t)fsize;
                char *buf = pin_xmalloc(replay + 1);
                lseek(log->fd, fsize - (off_t)replay, SEEK_SET);
                ssize_t got = read(log->fd, buf, replay);
                lseek(log->fd, 0, SEEK_END);
                if (got > 0) {
                    buf[got] = '\0';
                    /* Skip partial first line. */
                    char *line = (replay == (size_t)fsize) ? buf : memchr(buf, '\n', (size_t)got);
                    if (line && line != buf) line++;
                    long long max_seq = 0, max_gen = 1;
                    while (line && *line) {
                        char *next = memchr(line, '\n', (size_t)got - (size_t)(line - buf));
                        size_t llen = next ? (size_t)(next - line) : strlen(line);
                        const char *vp = NULL;
                        long long sv = 0, gv = 0;
                        if (llen > 0 && line[0] == '{' && pin_json_find_key(line, "seq", &vp)
                            && pin_json_parse_int(&vp, &sv)) {
                            if (sv > max_seq) max_seq = sv;
                        }
                        if (llen > 0 && line[0] == '{' && pin_json_find_key(line, "generation", &vp)
                            && pin_json_parse_int(&vp, &gv)) {
                            if (gv > max_gen) max_gen = gv;
                        }
                        if (sv > 0) {
                            /* Capture the most recent ring_capacity events. */
                            ring_slot *slot = &log->ring[log->head];
                            if (slot->json) slot_free(slot);
                            slot->seq = sv;
                            slot->json = pin_xstrndup(line, llen);
                            slot->json_len = llen;
                            log->head = (log->head + 1) % log->cap;
                            if (log->used < log->cap) log->used++;
                            log->newest_seq = sv;
                        }
                        if (!next) break;
                        line = next + 1;
                    }
                    log->generation = max_gen + 1;
                    if (log->newest_seq > 0) {
                        if (log->used >= log->cap) {
                            size_t oldest = (log->head + log->cap - log->used) % log->cap;
                            log->oldest_seq = log->ring[oldest].seq;
                        } else {
                            size_t oldest = (log->head + log->cap - log->used) % log->cap;
                            log->oldest_seq = log->ring[oldest].seq;
                        }
                    }
                }
                free(buf);
            }
        }
    }
    return log;
}

void pin_event_log_close(pin_event_log *log) {
    if (!log) return;
    pthread_rwlock_wrlock(&log->rw);
    log->closing = true;
    pin_subscriber *s = log->subs_head;
    while (s) {
        pin_subscriber *next = s->next;
        subs_unregister_unlocked(log, s);
        subs_close(s);
        s = next;
    }
    pthread_rwlock_unlock(&log->rw);
    if (log->fd >= 0) {
        fsync(log->fd);
        close(log->fd);
    }
    for (size_t i = 0; i < log->cap; i++) slot_free(&log->ring[i]);
    free(log->ring);
    pthread_rwlock_destroy(&log->rw);
    free(log);
}

/* ====================================================================
 * Append + bump
 * ==================================================================== */

void pin_event_log_append(pin_event_log *log, const char *kind,
                          const char *json, size_t json_len) {
    if (!log) return;
    pthread_rwlock_wrlock(&log->rw);
    long long seq = ++log->newest_seq;
    char *event_json = NULL;
    size_t event_len = 0;
    format_event_json(seq, log->generation, kind, json, json_len,
                      &event_json, &event_len);
    /* Push into ring. */
    ring_slot *slot = &log->ring[log->head];
    if (slot->json) slot_free(slot);
    slot->seq = seq;
    slot->json = event_json;       /* ownership transferred */
    slot->json_len = event_len;
    log->head = (log->head + 1) % log->cap;
    if (log->used < log->cap) log->used++;
    /* Maintain oldest_seq. */
    size_t oldest = (log->head + log->cap - log->used) % log->cap;
    log->oldest_seq = log->ring[oldest].seq;
    /* On-disk append. */
    log_writeln_locked(log, event_json, event_len);
    /* Broadcast. We hold the wrlock during sends, which serializes
     * appends — fine for our throughput bracket; a future optimization
     * is to drop to rdlock for the broadcast loop. */
    pin_subscriber *s = log->subs_head;
    while (s) {
        pin_subscriber *next = s->next;
        if (!s->dead) (void)sub_send_event(s, seq, event_json, event_len);
        s = next;
    }
    pthread_rwlock_unlock(&log->rw);
}

void pin_event_log_bump_generation(pin_event_log *log) {
    if (!log) return;
    pthread_rwlock_wrlock(&log->rw);
    log->generation++;
    /* Tell live subscribers. We piggyback on the broadcast path with a
     * synthetic event so they all see the new generation in order. */
    pin_buf payload;
    pin_buf_init(&payload);
    pin_buf_printf(&payload, "{\"reason\":\"bump\",\"generation\":%lld}", log->generation);
    long long seq = ++log->newest_seq;
    char *event_json = NULL;
    size_t event_len = 0;
    format_event_json(seq, log->generation, "cursor_reset",
                      payload.ptr, payload.len,
                      &event_json, &event_len);
    pin_buf_free(&payload);
    ring_slot *slot = &log->ring[log->head];
    if (slot->json) slot_free(slot);
    slot->seq = seq;
    slot->json = event_json;
    slot->json_len = event_len;
    log->head = (log->head + 1) % log->cap;
    if (log->used < log->cap) log->used++;
    size_t oldest = (log->head + log->cap - log->used) % log->cap;
    log->oldest_seq = log->ring[oldest].seq;
    log_writeln_locked(log, event_json, event_len);
    pin_subscriber *s = log->subs_head;
    while (s) {
        pin_subscriber *next = s->next;
        if (!s->dead) (void)sub_send_event(s, seq, event_json, event_len);
        s = next;
    }
    pthread_rwlock_unlock(&log->rw);
}

void pin_event_log_status_get(pin_event_log *log, pin_event_log_status *out) {
    if (!log || !out) return;
    pthread_rwlock_rdlock(&log->rw);
    out->generation = log->generation;
    out->newest_seq = log->newest_seq;
    out->oldest_seq = log->newest_seq == 0 ? 0 : log->oldest_seq;
    out->ring_used  = log->used;
    out->subscriber_count = log->subs_count;
    pthread_rwlock_unlock(&log->rw);
}

/* ====================================================================
 * Snapshot (compact rendering for the cursor-too-old fallback)
 * ====================================================================
 *
 * We walk the ring oldest -> newest, parse the per-event payload kind,
 * and emit one combined JSON object:
 *
 *   {
 *     "generation": N,
 *     "newest_seq": M,
 *     "events": [ <every ring event in order> ]
 *   }
 *
 * The UI uses this to repopulate the conversation when its cursor has
 * fallen out of the in-memory window. Disk replay will be a phase-4
 * upgrade. */

void pin_event_log_render_snapshot(pin_event_log *log, pin_buf *out) {
    if (!log || !out) return;
    pthread_rwlock_rdlock(&log->rw);
    pin_buf_putc(out, '{');
    pin_buf_printf(out, "\"generation\":%lld", log->generation);
    pin_buf_printf(out, ",\"newest_seq\":%lld", log->newest_seq);
    pin_buf_printf(out, ",\"oldest_seq\":%lld",
                   log->newest_seq == 0 ? 0 : log->oldest_seq);
    pin_buf_puts(out, ",\"events\":[");
    size_t start = (log->head + log->cap - log->used) % log->cap;
    bool first = true;
    for (size_t i = 0; i < log->used; i++) {
        size_t idx = (start + i) % log->cap;
        if (!log->ring[idx].json) continue;
        if (!first) pin_buf_putc(out, ',');
        pin_buf_append(out, log->ring[idx].json, log->ring[idx].json_len);
        first = false;
    }
    pin_buf_puts(out, "]}");
    pthread_rwlock_unlock(&log->rw);
}

/* Extract payload.text from a stored event JSON. Returns a malloc'd
 * string (caller frees) or NULL. */
static char *event_payload_text(const char *json) {
    const char *pp = NULL;
    if (!pin_json_find_key(json, "payload", &pp)) return NULL;
    const char *tp = NULL;
    if (!pin_json_find_key(pp, "text", &tp)) return NULL;
    char *text = NULL;
    if (!pin_json_parse_string(&tp, &text)) return NULL;
    return text;
}

void pin_event_log_last_preview(pin_event_log *log, pin_buf *user_out,
                                pin_buf *answer_out) {
    if (!log) return;
    pthread_rwlock_rdlock(&log->rw);
    bool have_user = false, have_ans = false;
    /* Walk newest -> oldest. */
    for (size_t i = 0; i < log->used && !(have_user && have_ans); i++) {
        size_t idx = (log->head + log->cap - 1 - i) % log->cap;
        const char *json = log->ring[idx].json;
        if (!json) continue;
        const char *kp = NULL;
        if (!pin_json_find_key(json, "kind", &kp)) continue;
        char *kind = NULL;
        if (!pin_json_parse_string(&kp, &kind)) continue;
        if (!have_user && !strcmp(kind, "user") && user_out) {
            char *t = event_payload_text(json);
            if (t) { pin_buf_puts(user_out, t); free(t); have_user = true; }
        } else if (!have_ans && !strcmp(kind, "answer") && answer_out) {
            char *t = event_payload_text(json);
            if (t) { pin_buf_puts(answer_out, t); free(t); have_ans = true; }
        }
        free(kind);
    }
    pthread_rwlock_unlock(&log->rw);
}

void pin_event_log_render_transcript(pin_event_log *log, pin_buf *out,
                                     size_t max_bytes) {
    if (!log || !out) return;
    pin_buf full;
    pin_buf_init(&full);
    pthread_rwlock_rdlock(&log->rw);
    size_t start = (log->head + log->cap - log->used) % log->cap;
    const char *prev = "";
    for (size_t i = 0; i < log->used; i++) {
        size_t idx = (start + i) % log->cap;
        const char *json = log->ring[idx].json;
        if (!json) continue;
        const char *kp = NULL;
        if (!pin_json_find_key(json, "kind", &kp)) continue;
        char *kind = NULL;
        if (!pin_json_parse_string(&kp, &kind)) continue;
        char *text = event_payload_text(json);
        if (!strcmp(kind, "user") && text) {
            if (full.len && full.ptr[full.len - 1] != '\n') pin_buf_putc(&full, '\n');
            pin_buf_puts(&full, "User: ");
            pin_buf_puts(&full, text);
            if (!full.len || full.ptr[full.len - 1] != '\n') pin_buf_putc(&full, '\n');
            prev = "user";
        } else if (!strcmp(kind, "answer") && text) {
            if (strcmp(prev, "answer") != 0) pin_buf_puts(&full, "Assistant: ");
            pin_buf_puts(&full, text);   /* answer chunks already carry newlines */
            prev = "answer";
        }
        free(kind);
        free(text);
    }
    pthread_rwlock_unlock(&log->rw);

    /* Bound to the most recent max_bytes, trimmed to a line boundary. */
    const char *s = full.ptr ? full.ptr : "";
    size_t n = full.len;
    if (max_bytes && n > max_bytes) {
        size_t off = n - max_bytes;
        while (off < n && s[off] != '\n') off++;
        if (off < n) off++;            /* skip the newline */
        s += off;
        n -= off;
    }
    if (n) pin_buf_append(out, s, n);
    pin_buf_free(&full);
}

/* ====================================================================
 * Subscriber attach + serve
 * ==================================================================== */

/* Apply resume semantics to the just-attached subscriber. Called inside
 * the subscriber thread BEFORE the live broadcast loop. Returns the
 * effective starting seq (subscriber will receive events with seq >
 * this value). */
static long long apply_resume(pin_event_log *log,
                              pin_subscriber *s,
                              long long client_generation,
                              long long after_seq) {
    pthread_rwlock_rdlock(&log->rw);
    long long gen = log->generation;
    long long oldest = log->oldest_seq;
    long long newest = log->newest_seq;
    pthread_rwlock_unlock(&log->rw);

    /* Generation mismatch -> emit cursor_reset, then a snapshot so the
     * client can rebuild its view, then live. */
    if (client_generation > 0 && client_generation != gen) {
        char buf[160];
        int n = snprintf(buf, sizeof(buf),
            "{\"reason\":\"generation_mismatch\",\"generation\":%lld,\"newest_seq\":%lld}",
            gen, newest);
        sub_send_event(s, -1, buf, (size_t)n);
        PIN_LOG_INFOF(PIN_EV_STREAM_CURSOR_RESET,
                      "client_gen=%lld server_gen=%lld",
                      client_generation, gen);
        after_seq = 0;  /* fall through to snapshot */
    }
    /* No prior cursor (after < 0) -> just live (no replay, no snapshot).
     * Used by smoke tests that only want to observe future activity. */
    if (after_seq < 0) {
        return newest;
    }
    /* after == 0 means "I'm fresh, give me what you have". Replay every
     * event currently in the ring so the UI can paint the conversation
     * before we go live. */
    if (after_seq == 0) {
        pthread_rwlock_rdlock(&log->rw);
        size_t start = (log->head + log->cap - log->used) % log->cap;
        for (size_t i = 0; i < log->used; i++) {
            size_t idx = (start + i) % log->cap;
            ring_slot *slot = &log->ring[idx];
            if (!slot->json) continue;
            sub_send_event(s, slot->seq, slot->json, slot->json_len);
        }
        pthread_rwlock_unlock(&log->rw);
        PIN_LOG_INFOF(PIN_EV_STREAM_RESUME,
                      "replay_all newest=%lld", newest);
        return newest;
    }
    /* Cursor ahead of newest -> cursor_reset. */
    if (after_seq > newest) {
        char buf[160];
        int n = snprintf(buf, sizeof(buf),
            "{\"reason\":\"future_cursor\",\"generation\":%lld,\"newest_seq\":%lld}",
            gen, newest);
        sub_send_event(s, -1, buf, (size_t)n);
        PIN_LOG_INFOF(PIN_EV_STREAM_CURSOR_RESET,
                      "after=%lld newest=%lld", after_seq, newest);
        return newest;
    }
    /* Inside ring -> replay. */
    if (after_seq >= oldest) {
        pthread_rwlock_rdlock(&log->rw);
        size_t start = (log->head + log->cap - log->used) % log->cap;
        for (size_t i = 0; i < log->used; i++) {
            size_t idx = (start + i) % log->cap;
            ring_slot *slot = &log->ring[idx];
            if (!slot->json) continue;
            if (slot->seq > after_seq && slot->seq <= newest) {
                sub_send_event(s, slot->seq, slot->json, slot->json_len);
            }
        }
        pthread_rwlock_unlock(&log->rw);
        PIN_LOG_INFOF(PIN_EV_STREAM_RESUME,
                      "after=%lld newest=%lld", after_seq, newest);
        return newest;
    }
    /* Cursor older than ring -> snapshot. */
    {
        pin_buf snap;
        pin_buf_init(&snap);
        pin_event_log_render_snapshot(log, &snap);
        pthread_mutex_lock(&s->fd_mu);
        if (s->kind == PIN_SUB_KIND_SSE) {
            pin_http_sse_emit(s->fd, -1, "snapshot", snap.ptr, snap.len);
        } else {
            pin_buf wrap;
            pin_buf_init(&wrap);
            pin_buf_puts(&wrap, "{\"kind\":\"snapshot\",\"payload\":");
            pin_buf_append(&wrap, snap.ptr, snap.len);
            pin_buf_putc(&wrap, '}');
            pin_ws_send_text(s->fd, wrap.ptr, wrap.len);
            pin_buf_free(&wrap);
        }
        pthread_mutex_unlock(&s->fd_mu);
        pin_buf_free(&snap);
        PIN_LOG_INFOF(PIN_EV_STREAM_SNAPSHOT_REQUIRED,
                      "after=%lld oldest=%lld", after_seq, oldest);
        return newest;
    }
}

/* Service one subscriber. For SSE, we just keepalive in a loop until
 * the producer marks us dead (write fails) or the client closes. For
 * WS, we additionally read frames from the client and dispatch them. */
void pin_event_log_serve_subscriber(pin_event_log *log,
                                    int fd,
                                    pin_sub_kind kind,
                                    long long client_generation,
                                    long long after_seq,
                                    pin_subscriber_callbacks cb) {
    pin_subscriber *s = subs_register(log, fd, kind);
    PIN_LOG_INFOF(PIN_EV_STREAM_OPEN, "kind=%d after=%lld",
                  (int)kind, after_seq);
    apply_resume(log, s, client_generation, after_seq);

    if (kind == PIN_SUB_KIND_SSE) {
        /* Park until dead. The producer holds fd_mu while writing. We
         * poll for client EOF/error so we react to disconnects within
         * a few ms; on the 15s tick we send a keepalive comment so
         * intermediaries don't time us out. */
        while (1) {
            struct pollfd pfd = {.fd = s->fd, .events = POLLIN};
            int pr = poll(&pfd, 1, 15000);
            pthread_rwlock_rdlock(&log->rw);
            bool dead = s->dead || log->closing;
            pthread_rwlock_unlock(&log->rw);
            if (dead) break;
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (pr > 0) {
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
                if (pfd.revents & POLLIN) {
                    /* SSE clients never send; any inbound byte (or EOF
                     * indicated by 0 from recv) means we should close. */
                    char tmp[64];
                    ssize_t n = recv(s->fd, tmp, sizeof(tmp), MSG_DONTWAIT);
                    if (n <= 0) break;
                    /* Drain and ignore. */
                    continue;
                }
            }
            /* timeout -> heartbeat. */
            pthread_mutex_lock(&s->fd_mu);
            bool ok = pin_http_sse_keepalive(s->fd);
            if (!ok) s->dead = true;
            pthread_mutex_unlock(&s->fd_mu);
            if (!ok) break;
        }
    } else {
        while (1) {
            int op = -1;
            uint8_t *payload = NULL;
            size_t   plen    = 0;
            if (!pin_ws_read_frame(s->fd, &op, &payload, &plen)) break;
            if (op == 0x8) { /* close */
                free(payload);
                break;
            }
            if (op == 0x9) { /* ping */
                pthread_mutex_lock(&s->fd_mu);
                pin_ws_send_pong(s->fd, payload, plen);
                pthread_mutex_unlock(&s->fd_mu);
                free(payload);
                continue;
            }
            if (op == 0xA) { /* pong */
                free(payload);
                continue;
            }
            if (op == 0x1) { /* text */
                /* Sanitize then dispatch. */
                pin_buf clean;
                pin_buf_init(&clean);
                pin_text_sanitize(&clean, (const char *)payload, plen, 64 * 1024);
                if (clean.ptr) {
                    /* Look at "kind": "input" | "control" */
                    const char *vp = NULL;
                    char *kind_str = NULL;
                    if (pin_json_find_key(clean.ptr, "kind", &vp) &&
                        pin_json_parse_string(&vp, &kind_str)) {
                        if (!strcmp(kind_str, "input") && cb.on_input) {
                            cb.on_input(cb.ud, clean.ptr, clean.len);
                        } else if (!strcmp(kind_str, "control") && cb.on_control) {
                            cb.on_control(cb.ud, clean.ptr, clean.len);
                        }
                        free(kind_str);
                    }
                }
                pin_buf_free(&clean);
                free(payload);
                continue;
            }
            /* Anything else is a protocol violation. */
            free(payload);
            pthread_mutex_lock(&s->fd_mu);
            pin_ws_send_close(s->fd, 1002);
            pthread_mutex_unlock(&s->fd_mu);
            break;
        }
    }

    /* Unregister + close. */
    pthread_rwlock_wrlock(&log->rw);
    subs_unregister_unlocked(log, s);
    pthread_rwlock_unlock(&log->rw);
    PIN_LOG_INFOF(PIN_EV_STREAM_CLOSE, "kind=%d", (int)kind);
    subs_close(s);
}
