#ifndef PIN_TRACESTREAM_H
#define PIN_TRACESTREAM_H

/* Parser for ds4-agent's --trace stream. The trace logs every token with
 * exact bytes (hex), flushed per token, plus structural markers. This
 * turns that into clean events:
 *
 *   - prefill/prompt tokens (bracketed by "tokens label=.. start/len")
 *     are skipped -- they are input, not output.
 *   - generation tokens are the model's output. Reasoning before
 *     "</think>" is reported as thinking; the rest is the answer.
 *   - raw DSML tool-call blocks in the answer are surfaced separately.
 *
 * Verified against a real trace: generation tokens reconstruct the exact
 * prose (see docs/transport-findings.md). Single-channel, structured --
 * this replaces un-rendering prose from the noisy TUI stdout. */

#include "util.h"

#include <stddef.h>

typedef struct {
    void *ud;
    void (*on_answer)   (void *ud, const char *text, size_t len);
    void (*on_thinking) (void *ud, const char *text, size_t len);
    void (*on_tool_call)(void *ud, const char *raw_dsml, size_t len);
    void (*on_turn)     (void *ud);   /* a new user turn began */
} pin_tracestream_cb;

typedef struct pin_tracestream pin_tracestream;

pin_tracestream *pin_tracestream_new(pin_tracestream_cb cb);
void             pin_tracestream_free(pin_tracestream *ts);

/* Feed one raw trace line (trailing newline optional). */
void pin_tracestream_feed_line(pin_tracestream *ts, const char *line, size_t len);

/* Emit any buffered tail (call at idle/turn-end so the last bytes flush). */
void pin_tracestream_flush(pin_tracestream *ts);

#endif
