# pinback

```text
browser / native webview shell
        |
        v
pinback-server
  |
  +-- embedded cockpit ui        web/index.html, web/app.js, shiki wasm
  +-- workspace catalog          ~/.pinback/workspaces.json
  +-- per-workspace event log    events.log jsonl + bounded replay ring
  +-- stream fanout              sse/websocket with seq + generation cursors
  +-- agent supervisor           one ds4-agent child, one active workspace
  +-- turn snapshots             private snapshot.git, diff, revert hunk api
```

pinback is a c99 cockpit for `ds4-agent`.

it is not a generic chat ui. it is a process supervisor, event log, and web control surface for a local coding agent. the hard part is not drawing the chat. the hard part is owning the child process, the stream cursor, the replay window, workspace switching, and the difference between "looks connected" and "the agent is actually alive".

## interesting parts

`src/agent.c` owns the child process. it forks `ds4-agent`, writes prompts to stdin, classifies stdout into user-visible events, watches stderr for the idle marker, snapshots the workspace at turn start, and emits a diff at turn end.

`src/event_log.c` is the durability and reconnect layer. every workspace has a jsonl log plus an in-memory ring. clients reconnect with `generation` and `seq`; stale cursors get replay, snapshot, or reset instead of silent duplication.

`src/workspace.c` is the catalog. it stores absolute workspace paths, labels, active id, session sha, per-workspace metadata, and hot event-log handles under `~/.pinback`.

`src/handlers.c` is the api surface: workspace create/activate/delete, input, control, revert, runtime, dashboard, health, readiness, metrics, and static ui serving.

`web/` is the shipped cockpit. it is embedded into `src/static_assets.c`, so the normal build has no separate frontend server.

`platform/` is native shell code. desktop shells can self-host `pinback-server`; mobile shells are thin webviews pointed at a remote pinback url.

`tools/ds4_agent_webpty.py` and `tools/ds4-agent-ui/` are the separate runtime cockpit/prototype path for owned ds4 profiles and public demo plumbing.

## build

```sh
make
make test
```

dev ui from disk:

```sh
./pinback-server --dev --agent-bin ./tools/fake-ds4-agent --workspace "$(pwd)"
```

refresh embedded ui:

```sh
make embed
make
```

## run

```sh
./pinback-server \
  --bind 127.0.0.1:8088 \
  --agent-bin /path/to/ds4-agent \
  --model /path/to/model.gguf \
  --workspace /absolute/project/path
```

open `http://127.0.0.1:8088`.

## api

```text
get     /api/w
post    /api/w
post    /api/w/<id>/activate
get     /api/w/<id>/events
post    /api/w/<id>/input
post    /api/w/<id>/control
post    /api/w/<id>/revert
delete  /api/w/<id>

get     /api/runtime
get     /api/dashboard
get     /healthz
get     /readyz
get     /metrics
```

## transport truth

default mode is clean non-interactive pipes plus transcript re-prefill when returning to a workspace. exact kv resume is behind `--kv-resume`: tui command channel for `/save` and `/switch`, trace tailing for clean prose and raw tool data.

read `docs/transport-findings.md` before changing transport.
