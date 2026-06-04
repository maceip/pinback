# DS4 Runtime Cockpit

Runtime is a DS4 runtime supervisor and cockpit, not a detached chat GUI.

The default relationship is an owned runtime profile: Runtime launches DS4
from an explicit profile, owns the process lifecycle, and shows the user the
exact DS4 command, working directory, model, backend, context, and contract
revision it is controlling.

Attach/adopt mode is allowed later, but it must be visibly weaker. If Runtime
did not launch the DS4 process, the UI must say `attached` and avoid pretending
that restart, save, health, or session state are fully owned.

## Product Rule

Every feature must strengthen one of these questions:

- What DS4 build am I running?
- What profile launched it?
- Where is the agent working?
- What model/backend/context is live?
- Is it healthy, waiting, generating, exited, or disconnected?
- Can I save, recover, or restart this session without guessing?

If a feature creates detached-GUI ambiguity, do not build it.

## Bucket Exit Gate

A bucket is not done after unit or regression harnesses alone. Every bucket
must end with a live smoke run that starts the relevant Runtime entrypoint as
a normal process and exercises the user-facing API or UI boundary with real
requests.

The live smoke result must be reported with the exact command. If a live smoke
cannot run, the bucket remains not done.

## Bucket 0: Runtime Ownership

Bucket 0 is the non-negotiable cockpit layer. Runtime must prove the DS4
process it controls before it presents chat, metrics, or recovery actions as
real.

- Preflight verifies DS4 root, executable, model path, and generated interface
  contract against the current DS4 git head.
- Startup takes an OS file lock for the runtime profile and writes one profile
  state file under `~/.ds4/runtime`.
- Port conflicts fail explicitly with the listening pid and command unless the
  existing listener is the same owned runtime profile.
- A second launch of the same owned profile reuses the existing bridge instead
  of spawning a duplicate runtime.
- Readiness is driven by DS4 agent output and a condition variable, not blind
  timer polling. The bridge should not advertise a usable runtime until the
  agent reaches its waiting state.
- Restart is available only for owned runtimes; attached runtimes must not
  pretend to have lifecycle authority.
- Runtime state records bridge pid, agent pid, command, cwd, DS4 `--chdir`,
  model, backend, context, DS4 head, contract revision, ownership, and ready
  state.
- Regression coverage lives in `runtime/test_ds4_webpty_supervisor.py` and uses a
  fake agent so the supervisor can be tested without loading a model.
- Bucket 0 live acceptance requires `runtime/ds4_bucket0_live_smoke.sh`: launch
  the real web PTY entrypoint outside the regression harness, observe readiness
  through `/health`, verify `/profile` and `/contract`, post one `/input`
  message, prove a same-profile second launch reuses the owned runtime instead
  of spawning a duplicate, and shut the smoke process down cleanly.

## Bucket 1: DS4 Interface Coverage

Bucket 1 is not a feature bucket. It is the contract map that keeps Runtime in
lockstep with the DS4 git head it is wrapping.

Runtime must know every current DS4 boundary it may need to expose, proxy, or
avoid. That includes:

- Server HTTP endpoints and model aliases from `ds4_server.c`, so Runtime can
  distinguish DS4 server mode from DS4 agent mode without guessing from docs.
- Agent slash commands from `ds4_agent.c`, so Runtime can offer session and KV
  actions only when DS4 actually supports them.
- Agent tool names and argument names from `ds4_agent.c`, so tool-call display,
  audit, and future structured controls do not invent a parallel vocabulary.
- Browser/CDP methods from `ds4_web.c`, so Runtime understands DS4's native web
  tool surface before adding browser shims or UI affordances.
- CLI flags across DS4 binaries, including distributed flags, so profiles can
  render exact launch commands and avoid hiding meaningful runtime settings.
- Environment variables and file interfaces, so Runtime can surface or protect
  real DS4 state such as KV sessions, history files, debug knobs, and browser
  integration points.

Bucket 1 is done only when the generated contract matches the current DS4 head,
the bridge refuses silent contract drift, and the live smoke proves the contract
is available from the running Runtime process.

## First Profile

The first supported profile is `agent-local-metal`.

The profile stores:

- DS4 root and git head.
- Generated DS4 interface contract revision.
- Ownership mode: `owned` or `attached`.
- Exact command and command cwd.
- DS4 `--chdir`.
- Model path.
- Backend, context, prefill chunk, and MTP state when known.
- Bridge host/port, DS4 pid, launch time, and health state.

## Platform Direction

Runtime is Mac-first, not Mac-only. The same profile primitive must be able to
grow into `agent-linux-cuda`, `server-local-metal`, `server-linux-cuda`, and
distributed coordinator profiles without rewriting the cockpit.
