# pinback

```text
browser or native webview shell
        |
        v
build/pinback-server
  c99 http server, embedded web ui, sse/ws event stream
        |
        +--> ~/.pinback
        |     workspaces.json
        |     workspaces/<id>/meta.json
        |     workspaces/<id>/events.log
        |     workspaces/<id>/snapshot.git
        |
        +--> one ds4-agent child
              stdin: prompts
              stdout: prose + rendered tool activity
              stderr: waiting marker as turn boundary
```

pinback is a local-first cockpit for `ds4-agent`.

the useful shape is simple: pick a directory, run one agent there, keep the transcript and workspace state, switch to another directory without pretending there are infinite agents or a hosted backend. the server is a single c binary with the web ui embedded into it. tls, auth, and public exposure live in front of it, usually caddy, tailscale, or wireguard.

## current shape

- `build/pinback-server` owns the workspace catalog, event logs, active agent process, and local http api.
- the web ui in `ui/app/` is embedded into `build/generated/static_assets.c`; use `make embed` after changing the ui.
- the default agent transport is clean `--non-interactive` pipes plus transcript re-prefill on workspace return.
- `--kv-resume` is the experimental exact resume path: tui-over-pipes for `/save` and `/switch`, with prose/tool data taken from `--trace`.
- each turn snapshots the workspace through a private shadow git dir and emits a revertable `turn_diff`.
- desktop shells in `platform/` are thin native webview launchers. mobile shells point at a remote pinback url.
- `runtime/` holds the separate ds4 runtime cockpit/prototype path (supervisor, web pty, vite ui).

## build

```sh
make
make test
```

to serve the checked-in ui from disk while editing:

```sh
./build/pinback-server --dev --agent-bin ./build/fake-ds4-agent --workspace "$(pwd)"
```

to refresh the embedded ui:

```sh
make embed
make
```

## run

```sh
./build/pinback-server \
  --bind 127.0.0.1:8088 \
  --agent-bin /path/to/ds4-agent \
  --model /path/to/model.gguf \
  --workspace /absolute/project/path
```

then open:

```text
http://127.0.0.1:8088
```

for a quick fake-agent smoke:

```sh
./build/pinback-server --agent-bin ./build/fake-ds4-agent --workspace "$(pwd)"
make smoke URL=http://127.0.0.1:8088
```

## api

```text
get    /api/w
post   /api/w                         create workspace
post   /api/w/<id>/activate           switch active workspace
get    /api/w/<id>/events             sse stream, replay, snapshot
post   /api/w/<id>/input              submit prompt
post   /api/w/<id>/control            abort or reset
post   /api/w/<id>/revert             apply reverse patch for a turn diff
delete /api/w/<id>

get    /api/runtime
get    /api/dashboard
get    /healthz
get    /readyz
get    /metrics
```

## repo map

```text
src/              c server, supervisor, event log, workspace store
build/            pinback-server, fake-ds4-agent, generated static_assets.c
ui/app/           shipped cockpit ui
ui/shiki-bundle/  shiki/oniguruma rebuild toolchain
tests/            c unit and integration tests
scripts/          embed + qa smoke harnesses
docs/             architecture, operations, postmortems, backlog
ingress/          caddy, tailscale, and wireguard examples
platform/         macos, ios, android, linux, and windows webview shells
runtime/          ds4 runtime supervisor + prototype ui (optional)
experiments/      transport probes (not shipped)
```

see [docs/REPO_LAYOUT.md](docs/REPO_LAYOUT.md) and [CONTRIBUTING.md](CONTRIBUTING.md).

## the edge that matters

do not call this tiny glue. pinback owns transport, streaming, session continuity, reconnect behavior, event replay, process lifecycle, workspace state, and public ingress boundaries. that is the hard part.

the default path is intentionally boring because it is the one that stays clean: non-interactive ds4 over pipes, with the previous transcript invisibly re-prefilled after a workspace switch. exact kv resume is behind `--kv-resume`; it is more interesting and more fragile, because it has to drive the tui command loop and parse trace output correctly.

read `docs/architecture/transport-findings.md` before changing the agent transport. that file is the current source of truth.
