#ifndef PIN_HTTP_H
#define PIN_HTTP_H

/* HTTP/1.1 server primitives.
 *
 * Borrowed in shape from ds4_server.c: blocking thread per connection,
 * one request per connection (no keep-alive — simpler, and our static
 * assets are small enough that pipelining doesn't matter), bounded
 * read sizes, SO_RCVTIMEO/SO_SNDTIMEO, MSG_NOSIGNAL/SO_NOSIGPIPE,
 * TCP_NODELAY for snappy SSE token frames.
 *
 * SSE writes go through pin_http_sse_emit, which uses writev(2) so the
 * id: line and data: line are one syscall.
 *
 * WebSocket support mirrors ds4_web.c framing primitives, server-side:
 * RFC 6455-strict frame parsing with masking required, control frames
 * <= 125 B, total frame size <= 1 MiB, no fragmentation. Reserved
 * opcodes / unmasked client frames close 1002.
 *
 * The whole module is the security boundary: every byte that crosses
 * it has been size-bounded, control-char-stripped (where applicable),
 * and JSON-escaped before being placed in any further structure. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util.h"

#define PIN_HTTP_MAX_HEADERS_BYTES (32  * 1024)
#define PIN_HTTP_MAX_BODY_BYTES    (8   * 1024 * 1024)
#define PIN_HTTP_IO_TIMEOUT_SEC    30

typedef struct {
    char  *method;
    char  *path;
    char  *query;
    char  *headers_blob;
    size_t headers_len;
    char  *body;
    size_t body_len;
    char   request_id[33];
} pin_request;

void pin_request_free(pin_request *r);

bool pin_http_read_request(int fd, pin_request *r);

bool pin_http_header(const pin_request *r, const char *name,
                     const char **out, size_t *out_len);

bool pin_http_query(const pin_request *r, const char *key,
                    const char **out, size_t *out_len);

/* Fixed-body response. r may be NULL to skip X-Request-Id (used for
 * pre-parse error replies). content_type may be NULL for 204. */
bool pin_http_respond(int fd, const pin_request *r, int status,
                      const char *content_type,
                      const char *body, size_t body_len);

bool pin_http_respond_text(int fd, const pin_request *r,
                           int status, const char *text);
bool pin_http_respond_json(int fd, const pin_request *r,
                           int status, const pin_buf *body);
/* Sanitized error JSON: {"code","message","request_id"}. */
bool pin_http_respond_error(int fd, const pin_request *r,
                            int status, const char *code,
                            const char *message);

/* SSE prologue. Caller emits events afterwards via pin_http_sse_emit and
 * closes the socket on disconnect. */
bool pin_http_begin_sse(int fd, const pin_request *r);

/* One SSE event. id < 0 omits `id:`. event may be NULL. data is sent
 * verbatim (callers JSON-encode if needed). */
bool pin_http_sse_emit(int fd, long long id, const char *event,
                       const char *data, size_t data_len);

/* `:\n\n` heartbeat. */
bool pin_http_sse_keepalive(int fd);

/* WebSocket upgrade. Validates Upgrade/Connection/Version/Key headers
 * and sends 101 with Sec-WebSocket-Accept = base64(sha1(key + GUID)). */
bool pin_ws_upgrade(int fd, const pin_request *r);

bool pin_ws_send_text (int fd, const char *data, size_t len);
bool pin_ws_send_close(int fd, uint16_t code);
bool pin_ws_send_pong (int fd, const uint8_t *payload, size_t len);

/* Read one frame. *out_payload is malloc'd (NUL-terminated). Caller frees.
 * Returns false on protocol error / disconnect; caller should send a
 * close frame and close the socket. */
bool pin_ws_read_frame(int fd, int *out_opcode,
                       uint8_t **out_payload, size_t *out_len);

#endif
