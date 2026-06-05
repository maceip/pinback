# pinback

```text
browser, phone, or native shell
        |
        |  local http, tailscale, caddy, wireguard
        v
build/pinback-server
  c99 supervisor + embedded workspace ui + sse/ws stream
        |
        +--> ~/.pinback
        |     workspaces.json
        |     workspaces/<id>/events.log
        |     workspaces/<id>/snapshot.git
        |
        +--> ds4-agent process
              stdin/stdout/stderr pipes
              transcript replay + turn diffs
```

pinback gives `ds4-agent` a remote-accessible workspace ui for multiple projects.

pick a directory, run one agent there, persist the transcript and workspace state, then switch to another project without pretending there are infinite agents or a hosted backend. the server is a single c binary with the web ui embedded into it. tls, auth, and public access live in front of it, usually caddy, tailscale, or wireguard.

## current implementation

- `build/pinback-server` is the c supervisor: workspace db, event log, agent lifecycle, embedded ui, health checks, and local http api.
- `ui/app/` compiles into `build/generated/static_assets.c`; run `make embed` after ui edits.
- agent i/o defaults to `--non-interactive` stdio with transcript replay when returning to a workspace.
- `--kv-resume` is experimental: it drives tui `/save` and `/switch` over pipes and reads prose/tool events from `--trace`.
- each turn writes a private shadow-git snapshot and a reversible `turn_diff`.
- `platform/` contains thin webview shells: desktop shells can self-host `pinback-server`; mobile shells connect to a remote url.

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
ui/app/           shipped workspace ui
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
