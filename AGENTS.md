# AGENTS.md

## Cursor Cloud specific instructions

### What this repo is

Pinback is a **DS4 Runtime Cockpit**: Python bridge (`tools/ds4_agent_webpty.py`) plus React UI (`tools/ds4-agent-ui`). It supervises an external **DS4** git checkout (`DS4_ROOT`), not a standalone chat backend.

### Required external dependency: DS4

Most commands need a real DS4 git tree whose `HEAD` matches `tools/ds4-agent-ui/ds4-interface-contract.json` (`ds4_revision`). On this VM, a matching checkout lives at `/workspace/.ds4` (cloned from `https://github.com/antirez/ds4` at the contract revision). Set:

```bash
export DS4_ROOT=/workspace/.ds4
```

If `DS4_ROOT` is missing or the revision drifts, the bridge exits with an explicit contract error unless you pass `--allow-contract-drift` (avoid for normal dev).

### Tooling

| Tool | Purpose |
|------|---------|
| `python3` | Bridge, contract tool, Bucket 0 tests (stdlib only) |
| `bun` | UI install/build/dev (`~/.bun/bin` on PATH after install) |
| `curl` | Live smoke script |
| `git` | DS4 checkout + contract extraction |
| `lsof` | Optional; port-conflict diagnostics in tests |

There is **no** repo-defined ESLint/Ruff; validation is the Bucket 0 harnesses below.

### Services (dev)

| Service | Command | Port |
|---------|---------|------|
| **Bridge (full stack)** | `python3 tools/ds4_agent_webpty.py --host 127.0.0.1 --port 18092 --ds4-root "$DS4_ROOT" --token "$DS4_WEBPTY_TOKEN"` | **18092** |
| **UI dev only** | `cd tools/ds4-agent-ui && bun run dev` | **18192** (no API proxy; use bridge origin for E2E) |

For bridge + UI without a GGUF, pass a fake agent after `--` (see `tools/ds4_bucket0_live_smoke.sh`).

Open cockpit: `http://127.0.0.1:18092/?token=<token>` (token from startup log or `DS4_WEBPTY_TOKEN`).

### Verify (recommended)

```bash
export DS4_ROOT=/workspace/.ds4
python3 tools/ds4_contract.py --ds4-root "$DS4_ROOT" --check tools/ds4-agent-ui/ds4-interface-contract.json
python3 tools/test_ds4_webpty_supervisor.py --ds4-root "$DS4_ROOT"
DS4_ROOT="$DS4_ROOT" tools/ds4_bucket0_live_smoke.sh
cd tools/ds4-agent-ui && bun run build
```

### Gotchas

- Default `DS4_ROOT` in code is `/Users/mac/ds4` (Mac-oriented); always export `DS4_ROOT` on Linux cloud VMs.
- Preflight requires the agent executable after `--` to exist; default launch expects `$DS4_ROOT/ds4-agent` and `$DS4_ROOT/ds4flash.gguf`.
- `bun run build` overwrites `tools/ds4-agent-ui/dist/`; the bridge serves that directory statically.
- Long-running bridge: use tmux; avoid embedding `--` in tmux `send-keys` without a wrapper script (argparse can see a lone `--` as the program name).

See `docs/ds4-runtime-cockpit.md` for product rules and bucket gates.
