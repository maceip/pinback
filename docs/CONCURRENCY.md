# Concurrency in pinback core

This document describes how the C core coordinates threads and which
invariants callers should rely on.

## Lock order

| Lock | Protects |
|------|----------|
| `pin_agent::api_mu` | single-flight `activate` / `submit` / `reset` / `abort` |
| `pin_agent::mu` | `state`, `active_id`, `active_path`, `bind_gen`, child pid, reader flags |
| `pin_agent::stdin_mu` | writes to child stdin |
| `pin_agent::sha_mu` | `captured_sha` (/save stdout ack) |
| `pin_workspace_store` rwlock | workspace metadata, lazy event-log open |
| `pin_event_log::rw` | ring buffer, subscriber list, on-disk append |

**Rules**

1. Never hold `a->mu` while calling `emit_*` helpers — they lock `mu` again
   (non-recursive mutex). Unlock first, or use `emit_event_snap` with a
   snapshot taken under the lock.
2. Never hold `a->mu` across blocking I/O (`waitpid`, `usleep`, disk poll).
   `kill_child()` drops `mu` before waiting.
3. Never persist workspace data using an unlocked `active_id`. Pin the
   workspace id under `mu` (see `kv_save_and_capture`) or use
   `agent_bind_snapshot` for event emission.
4. `sha_mu` is independent; do not nest it inside `mu` in the opposite
   order on code paths that already hold `sha_mu`.
5. Event logs: append under `wrlock`; subscriber broadcast runs after
   unlock so slow SSE/WebSocket clients do not block producers.

## Workspace binding (`bind_gen`)

Each successful workspace bind in `pin_agent_activate` increments
`bind_gen` and copies `active_id` / `active_path` under `mu`.

Reader threads (stdout, stderr, trace tailer) call `emit_*` helpers that:

1. Snapshot `ws_id` + `bind_gen` under `mu`.
2. Resolve the workspace event log.
3. Re-check `bind_gen` and `ws_id` before `pin_event_log_append`.

Stale events from a dying child during a switch are dropped instead of
landing in the next workspace's log.

## Thread lifecycle flags

`reader_running`, `err_running` — set/cleared under `mu`; waited on via
`pthread_cond_timedwait` in `wait_reader_threads`.

`trace_running` — `volatile sig_atomic_t`; set to `0` under `mu` (or in
`kill_child` before unlock) so the trace tailer loop can observe stop
without locking on every read.

## KV save / switch

- `/save` runs in `kv_save_and_capture` **without** `mu` (polls kvcache).
- Workspace id for persistence is pinned at entry.
- SHA from stdout is latched in `captured_sha` under `sha_mu` only;
  persistence uses the pinned workspace id, never a racy `active_id`.

## Intentional races avoided

| Former footgun | Mitigation |
|----------------|------------|
| `emit_event` read `active_id` unlocked | `bind_gen` snapshot + re-check |
| `kill_child` held `mu` during `usleep`/`waitpid` | `kill_child` drops `mu` while blocking |
| `session_sha` written from stdout during switch | persist only from pinned `ws_id` in save path |
| SSE broadcast under event-log `wrlock` | subscriber snapshot, broadcast after unlock |
| concurrent HTTP POST activate/input | `api_mu` on agent public API |

## HTTP handlers (`handlers.c` + `pinback.c`)

`pinback-server` spawns one detached thread per accepted TCP connection.
Each thread runs `pin_handle_connection` → one request → `close(fd)`.

| Endpoint | Store / log locks | Agent | Notes |
|----------|-------------------|-------|-------|
| `GET /api/w` | `list` rdlock | — | |
| `POST /api/w` | `create` wrlock | — | |
| `POST .../activate` | `get` rdlock | `activate` (api_mu) | may block for KV boot/switch |
| `POST .../input` | `get_active` rdlock | `submit` (api_mu) | 409 if not active; agent re-checks |
| `POST .../control` | `reset` wrlock | `abort`/`reset` (api_mu) | |
| `GET .../events` | `event_log` rdlock in serve | — | blocks thread until client disconnects |
| `GET /api/dashboard` | `list` rdlock, then per-ws `event_log` rdlock | `status_get` (mu only) | no store lock held during previews |
| `POST .../revert` | `get` rdlock, `event_log` append | — | filesystem revert outside locks |

**Audit result (2026-06):** workspace store and event logs are self-locking;
no handler holds `store.rw` across agent or SSE blocking calls. Agent
mutations are serialized with `api_mu` so overlapping activate/input from
different connections cannot interleave save/kill/spawn. `handle_ws_input`
still checks `get_active` before `submit` for a clear 409; with `api_mu`
that check is not racy against another activate on a different thread.

**Long SSE sessions** do not hold the workspace store lock; they only
take the per-log `rw` inside `pin_event_log_serve_subscriber`.
