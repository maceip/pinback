#ifndef PIN_HANDLERS_H
#define PIN_HANDLERS_H

/* Per-connection request handler. Routes one HTTP request from a fresh
 * client fd to the right endpoint and closes the fd. Called on a
 * detached pthread per accept() in pinback-server (see pinback.c).
 *
 * Concurrency:
 *   - pin_workspace_store_* APIs take the store rwlock internally.
 *   - pin_event_log_* APIs take each log's rwlock internally.
 *   - pin_agent_activate/submit/reset/abort are serialized via api_mu.
 *   - GET /api/w/<id>/events blocks this thread for the SSE session;
 *     other endpoints are short-lived. See docs/CONCURRENCY.md. */

#include "agent.h"
#include "workspace.h"

typedef struct {
    pin_workspace_store *ws; /* persistent workspace catalog */
    pin_agent *agent;        /* single ds4-agent supervisor */
    const char *web_root;    /* directory holding index.html etc. */
    const char *bind_str;    /* "127.0.0.1:8088" for diagnostics */
    long long started_ms;
    bool dev_mode; /* serve UI from disk, not embedded */
} pin_app;

void pin_handle_connection(pin_app *app, int fd);

#endif
