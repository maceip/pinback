#ifndef PIN_EVENT_LOG_H
#define PIN_EVENT_LOG_H

/* Authoritative event log + subscriber fan-out for the active session.
 *
 *   on-disk append-only file  <--  pin_event_log_append --|
 *                                                         |
 *   in-memory ring (bounded)  <-- (same call) ------------|
 *                                                         |
 *   for each subscriber:                                  |
 *     SSE: writev(id_line, "event: event\ndata: ", json, "\n\n")
 *     WS : one text frame containing the JSON event object
 *
 * Resume semantics, applied at subscriber-attach time:
 *   - generation mismatch (or client_generation > 0 not equal):
 *       emit cursor_reset, then live.
 *   - after > newest_seq:                                  cursor_reset.
 *   - oldest_seq <= after <= newest_seq:                   replay window.
 *   - after < oldest_seq:                                  snapshot.
 *   - after <= 0 / unset:                                  live only.
 *
 * Generation increments on:
 *   - log open (so old cursors after a restart get cursor_reset)
 *   - explicit pin_event_log_bump_generation (clear, upstream restart) */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util.h"

typedef enum {
    PIN_SUB_KIND_SSE,
    PIN_SUB_KIND_WS,
} pin_sub_kind;

typedef struct pin_event_log  pin_event_log;
typedef struct pin_subscriber pin_subscriber;

typedef struct {
    long long generation;
    long long oldest_seq;
    long long newest_seq;
    size_t    ring_used;
    size_t    subscriber_count;
} pin_event_log_status;

/* Open the log at `path`. If it exists, the most recent ring_capacity
 * events are loaded into the in-memory ring and oldest_seq/newest_seq
 * are restored; generation is set to (max generation seen + 1). */
pin_event_log *pin_event_log_open(const char *path, size_t ring_capacity);
void           pin_event_log_close(pin_event_log *log);

void pin_event_log_status_get(pin_event_log *log, pin_event_log_status *out);

/* Append one event. payload_json is a JSON value (object/string/number/
 * array). If payload_json_len is 0, the function does NOT strlen for
 * you — pass the explicit length. Thread-safe. */
void pin_event_log_append(pin_event_log *log,
                          const char *kind,
                          const char *payload_json, size_t payload_json_len);

/* Bump generation. Subscribers receive a synthetic cursor_reset event. */
void pin_event_log_bump_generation(pin_event_log *log);

/* Render compact conversation snapshot covering the ring. */
void pin_event_log_render_snapshot(pin_event_log *log, pin_buf *out);

/* Render the ring as a plain-text "User:/Assistant:" transcript suitable
 * for re-priming a freshly spawned agent (resume across workspace
 * switches). Appends to `out`. If the transcript exceeds `max_bytes`,
 * only the most recent tail (trimmed at a line boundary) is emitted. */
void pin_event_log_render_transcript(pin_event_log *log, pin_buf *out,
                                     size_t max_bytes);

/* Newest user prompt and newest answer text in the ring (for dashboard
 * previews). Each appends to its buf when found; bufs left empty if none. */
void pin_event_log_last_preview(pin_event_log *log, pin_buf *user_out,
                                pin_buf *answer_out);

/* Subscribe and serve. Takes ownership of fd. Returns when the client
 * disconnects, the log closes, or a protocol error occurs. */
typedef struct {
    void (*on_input)  (void *ud, const char *json, size_t len);
    void (*on_control)(void *ud, const char *json, size_t len);
    void  *ud;
} pin_subscriber_callbacks;

void pin_event_log_serve_subscriber(pin_event_log *log,
                                    int fd,
                                    pin_sub_kind kind,
                                    long long client_generation,
                                    long long after_seq,
                                    pin_subscriber_callbacks cb);

#endif
