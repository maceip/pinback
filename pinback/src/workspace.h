#ifndef PIN_WORKSPACE_H
#define PIN_WORKSPACE_H

/* Workspace catalog + per-workspace event log handles.
 *
 * v0 model (matches the user's `cd <dir> && claude` mental model):
 *   - One ds4-agent child process at any moment.
 *   - 1:1 conversation per workspace, with a "reset" affordance.
 *   - Switching the active workspace pauses the current agent (/save),
 *     kills it, respawns in the new --chdir; if the destination has a
 *     prior session_sha, the supervisor pipes /switch <sha> on stdin.
 *
 * Persistence layout (atomic writes, JSON Lines for the event log):
 *
 *   ~/.pinback/
 *     workspaces.json              catalog (this module owns it)
 *     active_id                    1-line file with the active workspace id
 *     workspaces/
 *       <id>/
 *         meta.json                mirror of catalog entry
 *         events.log               per-workspace pin_event_log file
 *     _trash/<id>/                 archived events.log on DELETE
 *
 * Concurrency: the store is guarded by an internal rwlock. Open event
 * logs are kept hot in a small map (one per workspace) so SSE clients
 * can attach without spinning on disk. */

#include "event_log.h"
#include "util.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char     *id;             /* "ws_<hex>" */
    char     *path;           /* absolute filesystem path */
    char     *label;          /* short display name */
    char     *session_sha;    /* last DS4 /save SHA, or NULL */
    uint64_t  created_ms;
    uint64_t  last_active_ms;
} pin_workspace_meta;

typedef struct pin_workspace_store pin_workspace_store;

/* Open or create the store rooted at `root_dir` (e.g. ~/.pinback).
 * Loads catalog + ensures dirs exist. Returns NULL on hard failure. */
pin_workspace_store *pin_workspace_store_open(const char *root_dir,
                                              size_t per_ws_ring_cap);
void                 pin_workspace_store_close(pin_workspace_store *s);

/* List all workspaces. Caller frees the returned array (and each entry's
 * strings via pin_workspace_meta_free). out_count may be 0. */
pin_workspace_meta *pin_workspace_store_list(pin_workspace_store *s,
                                             size_t *out_count);
void                pin_workspace_meta_free(pin_workspace_meta *m);
void                pin_workspace_meta_array_free(pin_workspace_meta *arr,
                                                  size_t n);

/* Look up by id. Caller frees via pin_workspace_meta_free.
 * Returns false if not found. */
bool pin_workspace_store_get(pin_workspace_store *s, const char *id,
                             pin_workspace_meta *out);

/* Create a new workspace. `path` must be a non-empty absolute path; we
 * realpath() it for safety. `label` may be NULL (defaults to basename).
 * On success, *out_id is malloc'd and owned by the caller. */
bool pin_workspace_store_create(pin_workspace_store *s,
                                const char *path, const char *label,
                                char **out_id, char **out_err);

/* Remove a workspace (archives its events.log). Returns false if id
 * is unknown or the workspace is currently active. */
bool pin_workspace_store_delete(pin_workspace_store *s, const char *id,
                                char **out_err);

/* Mark `id` as active. Persisted to disk. */
bool pin_workspace_store_set_active(pin_workspace_store *s, const char *id);
char *pin_workspace_store_get_active(pin_workspace_store *s); /* malloc'd or NULL */

/* Persist the latest /save SHA for a workspace. */
bool pin_workspace_store_set_session_sha(pin_workspace_store *s,
                                         const char *id, const char *sha);

/* Reset: clear session_sha + truncate events.log. Caller must ensure
 * the workspace is NOT bound to a live agent. Returns false on error. */
bool pin_workspace_store_reset(pin_workspace_store *s, const char *id,
                               char **out_err);

/* Get-or-open the per-workspace event log. The returned pointer is owned
 * by the store and stays valid until pin_workspace_store_close. */
pin_event_log *pin_workspace_store_event_log(pin_workspace_store *s,
                                             const char *id);

/* Write the per-workspace state directory (<root>/workspaces/<id>) into
 * `buf`. Returns false if buf is too small. Does not require the id to
 * exist in the catalog. */
bool pin_workspace_store_ws_dir(pin_workspace_store *s, const char *id,
                                char *buf, size_t cap);

#endif
