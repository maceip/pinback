#ifndef PIN_VTERM_H
#define PIN_VTERM_H

/* Minimal virtual terminal for driving ds4-agent's interactive (TUI)
 * mode over pipes. ds4-agent under LINENOISE_ASSUME_TTY renders prose +
 * tool lines through linenoise's cursor/redraw choreography; this models
 * the screen so the clean content (everything above the editor widget)
 * can be recovered. The algorithm is the validated reference in
 * experiments/transport-probe/vterm_ref.py (cross-checked against pyte).
 *
 * Escape subset handled: CR LF BS TAB, CSI A/B/C/D (cursor), G (column),
 * H/f (position), K/J (erase), m (SGR, ignored), ? private modes
 * (ignored), 6n (CPR request -> caller must reply). */

#include "util.h"

#include <stddef.h>

typedef struct pin_vterm pin_vterm;

pin_vterm *pin_vterm_new(int cols, int rows);
void pin_vterm_free(pin_vterm *vt);

/* Feed raw bytes from the agent's stdout. Returns the number of CPR
 * (ESC[6n) requests observed in this chunk; the caller must write
 * "\x1b[1;200R" to the agent's stdin that many times or the agent hangs. */
int pin_vterm_feed(pin_vterm *vt, const char *bytes, size_t n);

/* Append the full current content region (scrolled-off history + the
 * rows above the editor widget) to `out`, with one '\n' per row. Content
 * grows monotonically, so callers can stream by tracking the byte length
 * they last consumed. */
void pin_vterm_content(pin_vterm *vt, pin_buf *out);

/* Copy the current status-footer phase ("idle" | "reading" |
 * "generation" | "") into buf. */
void pin_vterm_status(pin_vterm *vt, char *buf, size_t cap);

#endif
