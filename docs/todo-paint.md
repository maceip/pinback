# Pinback — todo-paint (deferred work)

This document captures things v0 deliberately did NOT ship. Each item
is here so it doesn't get lost, and most of them have a small note on
what would unlock them. Order is roughly "what users will ask for
first."

## 1. Session resume across workspace switches  [unblocks #2, #3]

**What** — switch from workspace A → B → back to A and have the agent
pick up A's conversation where it left off. Today, switching tears the
agent down and respawns it fresh; the old session is gone.

**Why deferred** — ds4-agent's `--non-interactive` mode treats every
stdin line as a user prompt. Slash commands (`/save`, `/switch`,
`/clear`) are only honored in the interactive TUI loop. Issuing
`/save` over stdin pollutes the conversation rather than persisting a
session. `~/ds4` is read-only for pinback, so we can't add a
non-interactive command channel upstream.

**Plan** — pty-based supervisor variant. Open a pseudo-terminal
(`posix_openpt` / `grantpt` / `unlockpt`), spawn ds4-agent attached to
the pty *without* `--non-interactive` so it runs the TUI loop, and
drive it the way an expect script would. Output stream stays the same
(prose + DSML), but we now have a control channel for slash commands.
The architectural shape is already wired:
`workspace.session_sha`, `agent.save_timeout_ms`, the `/save`-await
helper, and the `/switch <sha>` post-spawn write all exist behind the
`save_timeout_ms > 0` gate.

**Risk** — linenoise sends/receives terminal escape codes; we have to
either disable line editing via stty or filter the in-band escapes
from the prose stream. Estimate: 1–2 days, including a contained
test for `/save → respawn → /switch` and a fuzz pass on the escape
filter.

## 2. Multi-agent (parallel workspaces)

**What** — run an agent in `~/house` and another in `~/pool` at the
same time, two browser tabs, both live.

**Why deferred** — RAM. One ds4-agent eats most of this laptop's
memory. The user explicitly scoped this out for v0 ("multi-agent not
as high a priority but still needed").

**Plan** — `agent.c` is already split per-workspace at the storage
layer, but `pin_agent` is a singleton. Promoting it to a map of
`(workspace_id → pin_agent_handle)` is mechanical once memory budgets
allow. Add an `--max-agents` flag (default 1) to keep the noob path
unchanged.

## 3. Per-workspace model selection

**What** — workspace A uses `ds4flash`, workspace B uses
`ds4-large`. Model path becomes part of `workspace.meta.json`.

**Why deferred** — same RAM constraint as #2, and we want to
stabilize the single-agent flow first.

**Plan** — surface `model_path` in the create-workspace API and
forward it through `pin_agent_config` on respawn.

## 4. Remote workspaces (`ssh://`, `sftp://`)

**What** — point a workspace at `ssh://host/path/to/repo` and have
pinback supervise a remote ds4-agent there. The browser UX is
unchanged.

**Why deferred** — non-trivial. Requires a remote agent broker, key
distribution, and bandwidth budgeting for stdout streams. v0's
workspace path is a local absolute string only.

**Plan** — first-pass design doc before any code. Most likely a
`pinback-relay` sidecar on the remote that exposes the same supervisor
API over a single mTLS connection.

## 5. Advanced diff viewer

**What** — render `tool_call` write/edit operations as side-by-side
diffs, inline accept/reject, hunk-level ignore, navigation. The user
called this out explicitly: "it will prob need the advanced diff
viewer code you mentioned."

**Why deferred** — the v0 instruction was "rock solid base first." The
DSML panel renders the call but does not interpret diff payloads.

**Plan** — bring back the prior diff renderer (it was deleted in the
ds4-server era), drive it from the parsed `tool_call` payload, and
keep the existing DSML panel as a fallback for unrecognized tools.

## 6. macOS `.app` wrapper  [explicitly on hold]

**What** — package `pinback-server` + the embedded UI as a
double-click `.app` that opens the browser to the local URL on launch.

**Why deferred** — explicit user instruction: "put all the mac
specific app stuff on hold but dont drop it."

**Plan** — minimal AppleScript launcher that runs the binary, waits
for `/healthz`, opens the default browser. Code-signing + notarization
optional for the dev preview.

## 7. Shiki bundle shrink

**What** — the embedded Shiki + Oniguruma WASM bundle is comfortable
but not small.

**Why deferred** — explicit user instruction: "dont worry about the
size of the bundles which is the right solution."

**Plan** — when it comes up, prune unused languages and themes from
`tools/web-build/` and re-run `gen-static-assets.sh`.

## 8. Full teleport export/import

**What** — pack a workspace (catalog row + `events.log` +
`session_sha`) into a single tarball that another pinback instance can
consume. Lets users move work between machines.

**Why deferred** — partly blocked by #1 (without session resume,
teleport is "history-only"), and partly out of scope for v0.

**Plan** — `pinback export <id>` and `pinback import <tarball>` CLI
subcommands. The on-disk shape is already 90% of the packet.

## 9. UX rails over the workspace primitive

**What** — onboarding affordances that explain "this app picks the
working directory, you don't `cd` first." User flagged that "people
arent used to this."

**Why deferred** — needs UI tutorial design once the v0 flow is
stable.

**Plan** — empty-state copy in the workspace picker, a one-line
"current dir: …" badge above the input, and a first-run explainer
panel.

## 10. Per-workspace agent settings (model temp, system prompt, etc.)

**What** — let the workspace persist a small bag of settings that get
applied on agent spawn.

**Why deferred** — v0 inherits whatever defaults the agent boots with.

**Plan** — extend `workspace.meta.json` with an optional `agent_args`
array, forward through `pin_agent_config.extra_argv`.

## 11. Auth / multi-user remote access

**What** — when pinback is reachable via Tailscale Funnel or a tunnel,
you want at least a shared secret or device check.

**Why deferred** — local-first v0; tunnels are documented but not
authenticated by pinback.

**Plan** — bearer token middleware in `handlers.c`, configurable via
`--auth-token` and `PINBACK_AUTH_TOKEN`.

## 12. Crash recovery for in-flight turns

**What** — if pinback crashes mid-turn, the next launch should mark
the in-flight turn as `interrupted` rather than leaving it dangling.

**Why deferred** — the on-disk `events.log` already contains enough
state to detect this (open `user` event with no matching
`answer_end`), but the recovery sweep is not wired.

**Plan** — on startup, scan each workspace's `events.log` and append a
synthetic `answer_end` with `payload.reason = "supervisor_restart"`
when we find a dangling turn.
