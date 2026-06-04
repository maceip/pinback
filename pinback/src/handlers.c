#include "handlers.h"

#include "agent.h"
#include "event_log.h"
#include "http.h"
#include "log.h"
#include "static_assets.h"
#include "util.h"
#include "workspace.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ====================================================================
 * Small helpers
 * ==================================================================== */

static char *header_dup(const pin_request *r, const char *name) {
    const char *v = NULL;
    size_t vlen = 0;
    if (!pin_http_header(r, name, &v, &vlen)) return NULL;
    return pin_xstrndup(v, vlen);
}

static char *query_dup(const pin_request *r, const char *key) {
    const char *v = NULL;
    size_t vlen = 0;
    if (!pin_http_query(r, key, &v, &vlen)) return NULL;
    return pin_xstrndup(v, vlen);
}

/* Path matching: returns true if `path` starts with `prefix` and the
 * next char is '/' or '\0'. Sets *out_rest to the remainder. */
static bool path_prefix(const char *path, const char *prefix, const char **out_rest) {
    size_t plen = strlen(prefix);
    if (strncmp(path, prefix, plen) != 0) return false;
    char c = path[plen];
    if (c != '\0' && c != '/') return false;
    if (out_rest) *out_rest = path + plen + (c == '/' ? 1 : 0);
    return true;
}

/* Extract the workspace id from "/api/w/<id>/<rest>". Returns NULL if
 * the path doesn't match; caller frees *out_id. *out_rest points into
 * `path` (no copy). */
static char *extract_ws_id(const char *path, const char **out_rest) {
    const char *after = NULL;
    if (!path_prefix(path, "/api/w", &after)) return NULL;
    if (*after == '\0') return NULL;
    /* Path is /api/w/<id>[/...]. */
    const char *slash = strchr(after, '/');
    size_t idlen = slash ? (size_t)(slash - after) : strlen(after);
    if (idlen == 0 || idlen > 32) return NULL;
    /* Validate: ws_<hex8>. */
    for (size_t i = 0; i < idlen; i++) {
        char c = after[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return NULL;
    }
    if (out_rest) *out_rest = slash ? slash + 1 : (after + idlen);
    return pin_xstrndup(after, idlen);
}

static void emit_workspace_json(pin_buf *b, const pin_workspace_meta *m) {
    pin_buf_putc(b, '{');
    pin_buf_puts(b, "\"id\":");      pin_json_str(b, m->id ? m->id : "");
    pin_buf_puts(b, ",\"path\":");   pin_json_str(b, m->path ? m->path : "");
    pin_buf_puts(b, ",\"label\":");  pin_json_str(b, m->label ? m->label : "");
    pin_buf_puts(b, ",\"session_sha\":");
    if (m->session_sha) pin_json_str(b, m->session_sha);
    else                pin_buf_puts(b, "null");
    pin_buf_printf(b, ",\"created_ms\":%llu", (unsigned long long)m->created_ms);
    pin_buf_printf(b, ",\"last_active_ms\":%llu", (unsigned long long)m->last_active_ms);
    pin_buf_putc(b, '}');
}

/* ====================================================================
 * /api/w  (GET list, POST create)
 * ==================================================================== */

static void handle_workspaces_root(pin_app *app, int fd, const pin_request *r) {
    if (!strcmp(r->method, "GET")) {
        size_t n = 0;
        pin_workspace_meta *list = pin_workspace_store_list(app->ws, &n);
        char *active = pin_workspace_store_get_active(app->ws);
        pin_buf out;
        pin_buf_init(&out);
        pin_buf_putc(&out, '{');
        pin_buf_puts(&out, "\"active_id\":");
        if (active) pin_json_str(&out, active); else pin_buf_puts(&out, "null");
        pin_buf_puts(&out, ",\"workspaces\":[");
        for (size_t i = 0; i < n; i++) {
            if (i) pin_buf_putc(&out, ',');
            emit_workspace_json(&out, &list[i]);
        }
        pin_buf_puts(&out, "]}");
        pin_http_respond_json(fd, r, 200, &out);
        pin_buf_free(&out);
        free(active);
        pin_workspace_meta_array_free(list, n);
        return;
    }
    if (!strcmp(r->method, "POST")) {
        if (!r->body || r->body_len == 0) {
            pin_http_respond_error(fd, r, 400, "empty_body", "JSON body required");
            return;
        }
        char *body = pin_xstrndup(r->body, r->body_len);
        const char *p = NULL;
        char *path = NULL, *label = NULL;
        if (!pin_json_find_key(body, "path", &p) ||
            !pin_json_parse_string(&p, &path)) {
            free(body);
            pin_http_respond_error(fd, r, 400, "missing_path",
                                   "expected {\"path\":...}");
            return;
        }
        const char *lp = NULL;
        if (pin_json_find_key(body, "label", &lp)) {
            (void)pin_json_parse_string(&lp, &label);
        }
        char *id = NULL, *err = NULL;
        bool ok = pin_workspace_store_create(app->ws, path, label, &id, &err);
        free(body); free(path); free(label);
        if (!ok) {
            pin_http_respond_error(fd, r, 400, "create_failed",
                                   err ? err : "could not create");
            free(err); free(id);
            return;
        }
        pin_workspace_meta meta;
        if (!pin_workspace_store_get(app->ws, id, &meta)) {
            free(id);
            pin_http_respond_error(fd, r, 500, "lookup_failed",
                                   "workspace vanished after create");
            return;
        }
        pin_buf out;
        pin_buf_init(&out);
        emit_workspace_json(&out, &meta);
        pin_http_respond_json(fd, r, 201, &out);
        pin_buf_free(&out);
        pin_workspace_meta_free(&meta);
        free(id);
        return;
    }
    pin_http_respond_error(fd, r, 405, "method_not_allowed",
                           "GET or POST required");
}

/* ====================================================================
 * /api/w/<id>/...
 * ==================================================================== */

static void handle_ws_activate(pin_app *app, int fd, const pin_request *r,
                               const char *id) {
    if (strcmp(r->method, "POST") != 0) {
        pin_http_respond_error(fd, r, 405, "method_not_allowed", "POST required");
        return;
    }
    char *err = NULL;
    if (!pin_agent_activate(app->agent, id, &err)) {
        pin_http_respond_error(fd, r, 400, "activate_failed",
                               err ? err : "could not activate");
        free(err);
        return;
    }
    pin_buf out;
    pin_buf_init(&out);
    pin_buf_printf(&out, "{\"ok\":true,\"active_id\":");
    pin_json_str(&out, id);
    pin_buf_putc(&out, '}');
    pin_http_respond_json(fd, r, 200, &out);
    pin_buf_free(&out);
}

static void handle_ws_events(pin_app *app, int fd, const pin_request *r,
                             const char *id) {
    if (strcmp(r->method, "GET") != 0) {
        pin_http_respond_error(fd, r, 405, "method_not_allowed", "GET required");
        return;
    }
    pin_event_log *log = pin_workspace_store_event_log(app->ws, id);
    if (!log) {
        pin_http_respond_error(fd, r, 404, "no_workspace", "unknown workspace id");
        return;
    }
    long long after = -1;
    long long generation = -1;
    char *last_id = header_dup(r, "Last-Event-ID");
    if (last_id) {
        char *e = NULL;
        long long v = strtoll(last_id, &e, 10);
        if (e != last_id) after = v;
        free(last_id);
    }
    char *qa = query_dup(r, "after");
    if (qa) {
        char *e = NULL;
        long long v = strtoll(qa, &e, 10);
        if (e != qa) after = v;
        free(qa);
    }
    char *qg = query_dup(r, "generation");
    if (qg) {
        char *e = NULL;
        long long v = strtoll(qg, &e, 10);
        if (e != qg) generation = v;
        free(qg);
    }
    if (!pin_http_begin_sse(fd, r)) return;
    pin_subscriber_callbacks cb = {0};
    pin_event_log_serve_subscriber(log, fd, PIN_SUB_KIND_SSE,
                                   generation, after, cb);
}

static void handle_ws_input(pin_app *app, int fd, const pin_request *r,
                            const char *id) {
    if (strcmp(r->method, "POST") != 0) {
        pin_http_respond_error(fd, r, 405, "method_not_allowed", "POST required");
        return;
    }
    if (!r->body || r->body_len == 0) {
        pin_http_respond_error(fd, r, 400, "empty_body", "JSON body required");
        return;
    }
    char *active = pin_workspace_store_get_active(app->ws);
    if (!active || strcmp(active, id) != 0) {
        free(active);
        pin_http_respond_error(fd, r, 409, "not_active",
                               "this workspace is not active");
        return;
    }
    free(active);
    char *body = pin_xstrndup(r->body, r->body_len);
    const char *p = NULL;
    char *text = NULL;
    if (!pin_json_find_key(body, "text", &p) ||
        !pin_json_parse_string(&p, &text)) {
        free(body);
        pin_http_respond_error(fd, r, 400, "missing_text", "expected {\"text\":...}");
        return;
    }
    if (text[0] == '\0') {
        free(text); free(body);
        pin_http_respond_error(fd, r, 400, "empty_text", "text must be non-empty");
        return;
    }
    char *err = NULL;
    bool ok = pin_agent_submit(app->agent, id, text, &err);
    free(text); free(body);
    if (!ok) {
        pin_http_respond_error(fd, r, 409, "rejected",
                               err ? err : "submit rejected");
        free(err);
        return;
    }
    pin_buf out;
    pin_buf_init(&out);
    pin_buf_puts(&out, "{\"ok\":true}");
    pin_http_respond_json(fd, r, 202, &out);
    pin_buf_free(&out);
}

static void handle_ws_control(pin_app *app, int fd, const pin_request *r,
                              const char *id) {
    if (strcmp(r->method, "POST") != 0) {
        pin_http_respond_error(fd, r, 405, "method_not_allowed", "POST required");
        return;
    }
    if (!r->body || r->body_len == 0) {
        pin_http_respond_error(fd, r, 400, "empty_body", "JSON body required");
        return;
    }
    char *body = pin_xstrndup(r->body, r->body_len);
    const char *p = NULL;
    char *op = NULL;
    if (!pin_json_find_key(body, "op", &p) ||
        !pin_json_parse_string(&p, &op)) {
        free(body);
        pin_http_respond_error(fd, r, 400, "missing_op", "expected {\"op\":...}");
        return;
    }
    bool ok = false;
    char *err = NULL;
    if (!strcmp(op, "abort")) {
        pin_agent_abort(app->agent);
        ok = true;
    } else if (!strcmp(op, "reset")) {
        ok = pin_agent_reset(app->agent, id, &err);
    } else {
        pin_http_respond_error(fd, r, 400, "bad_op", "unknown control op");
        free(op); free(body);
        return;
    }
    free(op); free(body);
    if (!ok) {
        pin_http_respond_error(fd, r, 400, "rejected",
                               err ? err : "control op rejected");
        free(err);
        return;
    }
    pin_buf out;
    pin_buf_init(&out);
    pin_buf_puts(&out, "{\"ok\":true}");
    pin_http_respond_json(fd, r, 200, &out);
    pin_buf_free(&out);
}

static void handle_ws_delete(pin_app *app, int fd, const pin_request *r,
                             const char *id) {
    if (strcmp(r->method, "DELETE") != 0) {
        pin_http_respond_error(fd, r, 405, "method_not_allowed",
                               "DELETE required");
        return;
    }
    char *err = NULL;
    if (!pin_workspace_store_delete(app->ws, id, &err)) {
        pin_http_respond_error(fd, r, 400, "delete_failed",
                               err ? err : "could not delete");
        free(err);
        return;
    }
    pin_buf out;
    pin_buf_init(&out);
    pin_buf_puts(&out, "{\"ok\":true}");
    pin_http_respond_json(fd, r, 200, &out);
    pin_buf_free(&out);
}

static void handle_ws_dispatch(pin_app *app, int fd, const pin_request *r) {
    const char *rest = NULL;
    char *id = extract_ws_id(r->path, &rest);
    if (!id) {
        /* /api/w (root) */
        handle_workspaces_root(app, fd, r);
        return;
    }
    /* Verify the workspace exists, except for DELETE which has its own check. */
    pin_workspace_meta got;
    bool exists = pin_workspace_store_get(app->ws, id, &got);
    if (exists) pin_workspace_meta_free(&got);
    if (!exists && strcmp(r->method, "DELETE") != 0) {
        free(id);
        pin_http_respond_error(fd, r, 404, "no_workspace", "unknown workspace id");
        return;
    }
    if (!*rest) {
        /* /api/w/<id> -> meta GET. */
        if (strcmp(r->method, "GET") == 0) {
            pin_workspace_store_get(app->ws, id, &got);
            pin_buf out;
            pin_buf_init(&out);
            emit_workspace_json(&out, &got);
            pin_http_respond_json(fd, r, 200, &out);
            pin_buf_free(&out);
            pin_workspace_meta_free(&got);
        } else if (strcmp(r->method, "DELETE") == 0) {
            handle_ws_delete(app, fd, r, id);
        } else {
            pin_http_respond_error(fd, r, 405, "method_not_allowed",
                                   "GET/DELETE required");
        }
        free(id);
        return;
    }
    if (!strcmp(rest, "activate"))      handle_ws_activate(app, fd, r, id);
    else if (!strcmp(rest, "events"))   handle_ws_events  (app, fd, r, id);
    else if (!strcmp(rest, "input"))    handle_ws_input   (app, fd, r, id);
    else if (!strcmp(rest, "control"))  handle_ws_control (app, fd, r, id);
    else pin_http_respond_error(fd, r, 404, "not_found", "no such workspace endpoint");
    free(id);
}

/* ====================================================================
 * /api/runtime, /healthz, /readyz, /metrics
 * ==================================================================== */

static void handle_runtime(pin_app *app, int fd, const pin_request *r) {
    pin_agent_status as;
    pin_agent_status_get(app->agent, &as);
    char *active = pin_workspace_store_get_active(app->ws);

    pin_buf out;
    pin_buf_init(&out);
    pin_buf_putc(&out, '{');
    pin_buf_puts(&out, "\"version\":\"pinback-server/0.2\",");
    pin_buf_printf(&out, "\"uptime_ms\":%lld,",
                   (long long)pin_monotonic_ms() - app->started_ms);
    pin_buf_puts(&out, "\"agent\":{");
    pin_buf_puts(&out, "\"state\":");
    pin_json_str(&out, pin_agent_state_name(as.state));
    pin_buf_printf(&out, ",\"pid\":%d", (int)as.child_pid);
    pin_buf_printf(&out, ",\"turns\":%lld,\"restarts\":%lld",
                   as.turns_total, as.restarts_total);
    pin_buf_puts(&out, ",\"workspace\":");
    if (as.active_workspace_id[0]) {
        pin_buf_putc(&out, '{');
        pin_buf_puts(&out, "\"id\":");
        pin_json_str(&out, as.active_workspace_id);
        pin_buf_puts(&out, ",\"path\":");
        pin_json_str(&out, as.active_workspace_path);
        pin_buf_putc(&out, '}');
    } else {
        pin_buf_puts(&out, "null");
    }
    pin_buf_puts(&out, "},\"active_id\":");
    if (active) pin_json_str(&out, active); else pin_buf_puts(&out, "null");
    pin_buf_putc(&out, '}');
    free(active);
    pin_http_respond_json(fd, r, 200, &out);
    pin_buf_free(&out);
}

static void handle_health(pin_app *app, int fd, const pin_request *r) {
    (void)app;
    pin_http_respond_text(fd, r, 200, "ok\n");
}

static void handle_ready(pin_app *app, int fd, const pin_request *r) {
    pin_agent_status as;
    pin_agent_status_get(app->agent, &as);
    bool ready = (as.state == PIN_AGENT_STATE_READY ||
                  as.state == PIN_AGENT_STATE_BUSY);
    pin_http_respond_text(fd, r, ready ? 200 : 503,
                          ready ? "ready\n" : "not ready\n");
}

static void handle_metrics(pin_app *app, int fd, const pin_request *r) {
    pin_agent_status as;
    pin_agent_status_get(app->agent, &as);
    pin_buf out;
    pin_buf_init(&out);
    pin_buf_printf(&out,
        "# TYPE pinback_uptime_seconds gauge\n"
        "pinback_uptime_seconds %.3f\n"
        "# TYPE pinback_agent_state gauge\n"
        "pinback_agent_state{state=\"%s\"} 1\n"
        "# TYPE pinback_agent_turns_total counter\n"
        "pinback_agent_turns_total %lld\n"
        "# TYPE pinback_agent_restarts_total counter\n"
        "pinback_agent_restarts_total %lld\n",
        (double)((long long)pin_monotonic_ms() - app->started_ms) / 1000.0,
        pin_agent_state_name(as.state),
        as.turns_total, as.restarts_total);
    pin_http_respond_text(fd, r, 200, out.ptr ? out.ptr : "");
    pin_buf_free(&out);
}

/* ====================================================================
 * Static asset routing
 * ==================================================================== */

static const char *content_type_for_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".css"))  return "text/css; charset=utf-8";
    if (!strcmp(dot, ".js") || !strcmp(dot, ".mjs"))
        return "application/javascript; charset=utf-8";
    if (!strcmp(dot, ".wasm")) return "application/wasm";
    if (!strcmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcmp(dot, ".png"))  return "image/png";
    if (!strcmp(dot, ".ico"))  return "image/x-icon";
    if (!strcmp(dot, ".json")) return "application/json";
    return "application/octet-stream";
}

static bool serve_from_disk(pin_app *app, int fd, const pin_request *r,
                            const char *path) {
    if (!app->web_root || !*app->web_root) return false;
    if (strstr(path, "..")) return false;
    const char *rel = (strcmp(path, "/") == 0) ? "/index.html" : path;
    char full[4096];
    if (snprintf(full, sizeof(full), "%s%s", app->web_root, rel) >= (int)sizeof(full))
        return false;
    int ffd = open(full, O_RDONLY);
    if (ffd < 0) return false;
    struct stat st;
    if (fstat(ffd, &st) < 0 || !S_ISREG(st.st_mode)) { close(ffd); return false; }
    pin_buf body;
    pin_buf_init(&body);
    char tmp[4096];
    ssize_t n;
    while ((n = read(ffd, tmp, sizeof(tmp))) > 0) pin_buf_append(&body, tmp, (size_t)n);
    close(ffd);
    pin_http_respond(fd, r, 200, content_type_for_ext(full),
                     body.ptr, body.len);
    pin_buf_free(&body);
    return true;
}

static void handle_static(pin_app *app, int fd, const pin_request *r) {
    const char *path = r->path;
    if (app->dev_mode && serve_from_disk(app, fd, r, path)) return;
    const pin_static_file *f = pin_static_lookup(path);
    if (!f && strcmp(path, "/index.html") == 0) f = pin_static_lookup("/");
    if (!f) {
        if (serve_from_disk(app, fd, r, path)) return;
        pin_http_respond_error(fd, r, 404, "not_found", "no such resource");
        return;
    }
    pin_http_respond(fd, r, 200, f->content_type,
                     (const char *)f->data, f->len);
}

/* ====================================================================
 * Dispatcher
 * ==================================================================== */

void pin_handle_connection(pin_app *app, int fd) {
    pin_request req;
    memset(&req, 0, sizeof(req));
    if (!pin_http_read_request(fd, &req)) {
        pin_request_free(&req);
        close(fd);
        return;
    }

    PIN_LOG_INFOF(PIN_EV_HTTP_REQUEST, "%s %s rid=%s",
        req.method ? req.method : "?",
        req.path   ? req.path   : "?",
        req.request_id);

    if      (path_prefix(req.path, "/api/w", NULL))
        handle_ws_dispatch(app, fd, &req);
    else if (!strcmp(req.path, "/api/runtime"))  handle_runtime (app, fd, &req);
    else if (!strcmp(req.path, "/healthz"))      handle_health  (app, fd, &req);
    else if (!strcmp(req.path, "/readyz"))       handle_ready   (app, fd, &req);
    else if (!strcmp(req.path, "/metrics"))      handle_metrics (app, fd, &req);
    else                                         handle_static  (app, fd, &req);

    pin_request_free(&req);
    close(fd);
}
