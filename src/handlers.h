#ifndef PIN_HANDLERS_H
#define PIN_HANDLERS_H

/* Per-connection request handler. Routes one HTTP request from a fresh
 * client fd to the right endpoint and closes the fd. Called on a
 * blocking thread spawned by main. */

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
