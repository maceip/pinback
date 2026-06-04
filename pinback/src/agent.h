#ifndef PIN_AGENT_H
#define PIN_AGENT_H

/* ds4-agent process supervisor.
 *
 * v0 contract:
 *   - Exactly one ds4-agent child process at any moment.
 *   - The active workspace's --chdir is passed at spawn time.
 *   - Stdout is parsed into structured events appended to the active
 *     workspace's pin_event_log: prose chunks, DSML tool_calls, agent
 *     control lines (saved-sha, switched-to, ready, etc.).
 *   - User input lines arrive via pin_agent_submit and are written to
 *     the child's stdin verbatim.
 *   - Switching the active workspace runs the save/kill/respawn dance:
 *
 *       1. write "/save\n" to stdin, capture SHA from stdout (best-effort)
 *       2. SIGTERM, wait, SIGKILL on timeout
 *       3. drain reader threads
 *       4. spawn fresh agent with new --chdir
 *       5. write "/switch <sha>\n" if the new workspace has a prior SHA
 *
 * The supervisor never assumes a real ds4-agent exists; for tests the
 * `--agent-bin` flag points at fake-ds4-agent. */

#include "event_log.h"
#include "util.h"
#include "workspace.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef enum {
    PIN_AGENT_STATE_IDLE = 0,    /* no child; no active workspace */
    PIN_AGENT_STATE_SPAWNING,    /* child forked, waiting for ready */
    PIN_AGENT_STATE_READY,       /* idle, ready to take input */
    PIN_AGENT_STATE_BUSY,        /* a turn is in flight */
    PIN_AGENT_STATE_DRAINING,    /* shutting down or switching */
    PIN_AGENT_STATE_DEAD,        /* child crashed; awaiting restart */
} pin_agent_state;

typedef struct {
    const char *agent_bin;       /* e.g. "ds4-agent" or absolute path */
    const char *model_path;      /* optional --model arg, may be NULL */
    int         spawn_ready_ms;  /* default 5000 */
    int         save_timeout_ms; /* default 5000 */
    int         term_timeout_ms; /* default 3000 */
    bool        kv_resume;       /* drive the TUI for exact /save + /switch
                                  * KV session resume (default off: clean
                                  * non-interactive transport + re-prefill) */
} pin_agent_config;

typedef struct pin_agent pin_agent;

pin_agent *pin_agent_new(const pin_agent_config *cfg, pin_workspace_store *ws);
void       pin_agent_free(pin_agent *a);

/* Activate a workspace. If the agent is already bound to a different
 * workspace, runs the save/kill/respawn dance synchronously. Returns
 * false on hard failure (out of memory, fork failed, etc.). */
bool pin_agent_activate(pin_agent *a, const char *workspace_id, char **out_err);

/* Submit one user prompt to the active workspace. Returns false if the
 * agent is not ready or no workspace is active. */
bool pin_agent_submit(pin_agent *a, const char *workspace_id,
                      const char *user_text, char **out_err);

/* Reset: kills the child, truncates the workspace's events.log via the
 * store, and respawns fresh in the same dir. */
bool pin_agent_reset(pin_agent *a, const char *workspace_id, char **out_err);

/* Abort the in-flight turn. Sends SIGINT to the child. No-op if idle. */
void pin_agent_abort(pin_agent *a);

typedef struct {
    pin_agent_state state;
    char            active_workspace_id[64];
    char            active_workspace_path[1024];
    pid_t           child_pid;
    long long       last_spawn_ms;
    long long       turns_total;
    long long       restarts_total;
} pin_agent_status;

void pin_agent_status_get(pin_agent *a, pin_agent_status *out);

const char *pin_agent_state_name(pin_agent_state s);

#endif
