# GUI for ds4-agent — open-aperture take

You asked for full aperture. This isn't a v1 spec. It's my honest read
of where a GUI for ds4-agent can actually move the needle, where it
can't, and what a non-vanity product positioning looks like.

I'll be direct about taste. Where I think something is a tar pit I
say so. Where I think people will ask for something but I'd push back,
I say that too.

## Section 0 — What ds4-agent's CLI is, as a baseline

So the comparisons are honest. ds4-agent's UX today, from reading
`~/ds4/ds4_agent.c`:

- TUI built on linenoise. One persistent prompt `ds4-agent> `, a
  status footer with tokens/sec and power.
- Conversations stream as plain prose. Tool calls are emitted as DSML
  blocks inline with the prose. Tool results are inline. Long
  markdown comes out as ANSI-colored text in the terminal.
- Tools available include shell (bash), file read/write/edit, and
  some web/browser bits. Edits write to disk *immediately* — there is
  no preview-then-apply.
- KV cache lives in `~/.ds4/kvcache/<sha>.kv`. `/save` snapshots the
  current session and prints the 8-char SHA prefix. `/switch <prefix>`
  loads. `/list` lists. There's no metadata beyond the SHA — no
  label, no last-touched-dir, no description.
- Working directory is wherever you launched the binary. There is no
  notion of a "workspace." The `--chdir` flag exists but is a foot-gun
  for noobs because they have to remember to use it.
- Long answers and large tool outputs scroll past. There is no
  navigation. Search is `<your terminal's> Ctrl-F`.
- Ctrl-C aborts the current turn. Ctrl-D exits. There's no undo of
  changes the agent made on disk.
- No notifications. If a long refactor finishes while you're in
  another window, you find out by tabbing back.
- No multi-agent. One process per terminal, and on this laptop you
  can only afford one period.
- No sharing. Conversations are SHA blobs in your home dir.

That's the bar. Anything a GUI does has to either remove an actual
pain point above, or unlock something that the terminal medium just
can't do, or it's vanity.

## Section 1 — The five things I think genuinely move the needle

Ordered by impact-to-effort, my honest taste, not a survey.

### 1. Tool calls as first-class inspectable cards, not inline text

In the terminal, a `bash` call is interleaved with prose. Long output
buries itself in the scrollback. `read_file` outputs dump the entire
file mid-conversation. `edit_file` shows you a unified diff once and
then it's gone.

In a GUI: each tool call renders as a card. Header has tool name,
status (running / ok / error / aborted), short args, exit code,
duration. Body is collapsed by default. Click to expand: full args,
full output, copy-to-clipboard. For `edit_file` the body is a side-by-
side diff, not a unified diff. For `read_file` the body is the file
content with a "show in tree" link. For `bash` the body is the exit
code + the streaming output rendered with ANSI.

Why this matters: it converts a 200-line agent run from "wall of
mixed prose and shell" into a thing you can scan. Claude Code does
this halfway. Cursor's chat does it well. ds4-agent today does not
do it at all.

We already have most of the plumbing — DSML is parsed, per-segment
bubbles exist. The work is rendering, not pipelining.

### 2. Edits as a pending-changeset, with hunk-level accept/reject

ds4-agent edits files immediately. If the agent makes a bad edit, you
revert it from git or by asking the agent to re-edit. There is no
checkpoint between "agent decided to write" and "bytes hit disk."

A GUI with a real changeset model: when the agent fires `edit_file`
or `write_file`, pinback intercepts the proposed bytes, holds them in
a staging area, and tells the agent "applied" so the conversation
flow continues. The user sees a sticky panel: "Pinback is holding 7
edits across 4 files. Review." Clicking opens a diff with per-hunk
accept/reject. Anything not accepted is rolled back to the original
content before the next agent turn touches the same files.

This is the single biggest UX leverage point because it changes the
trust model for using a coding agent at all.

It is also the highest-risk feature in this list. You need to know
when to apply (e.g. before bash that depends on the file), the agent
will sometimes get confused if it sees its edits as applied but the
file on disk doesn't match — the right model is to apply to a
shadow filesystem layer and let the agent see its own writes
consistently. That's not trivial but it's also not impossible. ds4-
agent reads/writes through a tool layer, so we can put the shadow at
that boundary in pinback if we own the bridge.

(This conflicts with `~/ds4` being read-only. We'd need either a
ds4-agent flag that lets pinback intercept tool I/O, or pinback
intercepts the actual filesystem syscalls of the child via a fuse
mount, or we accept that this requires upstream cooperation.)

### 3. A timeline you can scrub, not a scroll buffer

Long agent runs — say a refactor that touches 30 files and runs 50
tool calls — become unscrollable in any chat UI. The right primitive
is a left rail that shows turns 1..N with one-line previews and
status icons. Click to jump. Each turn is collapsible. Within a
turn, each tool call is collapsible.

This is shipped pieces in v0 already (per-segment bubbles). It just
needs a navigation rail layered on top.

### 4. Cross-workspace dashboard

The CLI has no answer to "where am I, across all my projects?" Even
listing saved sessions (`/list`) shows you SHA prefixes, not "the
website refactor I started on Tuesday in `~/site`."

A GUI dashboard: rows for each workspace with last-active time, last
turn's first 80 chars, current state (idle / running / error),
working directory, last edit. Sort by recency. Click to enter.

This is the "feels like a real product" view. Cheap to build on top
of `workspaces.json` + per-workspace `events.log`.

### 5. Background-friendly turns with notification

Two patterns that don't fit the terminal:

- "Run this and tell me when it's done." Browser notification
  (or pinback emits a webhook / writes to a system notifier) on
  `answer_end` if the user has tabbed away.
- "Run this overnight, I'll check in the morning." A long task
  doesn't fail because the laptop slept; pinback queues input,
  resumes when the agent is ready. The events.log holds the
  transcript regardless of whether anyone's watching.

Neither needs ds4-agent changes. Both are first-class GUI features.

## Section 2 — The "people will ask for this but I'd push back" list

I want to be opinionated here. Some of these are real but should not
be on the early roadmap. Some are real but for a different product.

### Branching / forking conversations

"Take turn 12, change the prompt, replay from there, keep the old
branch." Genuinely useful for prompt iteration. ds4-agent's KV cache
SHA model would technically support it (each branch is a new chain
of saves). But — the workflow has thin daily value for the type of
user who wants a GUI to ds4-agent. The CLI users who care about this
already use git for it. I'd defer.

### Image / multimodal input

ds4-agent is text in / text out. Adding multimodal in the GUI without
upstream support is impersonating a capability the model doesn't
have. Don't do it.

### Multi-agent / parallel sessions

You said RAM constrains this on the dev machine. The architecture
should *not* preclude it but the v1 product should not *promote* it.
The honest answer is "one agent, switch fast." Pretending we have
multi-agent invites bug reports we can't service.

### IDE-style autocomplete, jump-to-definition, project search

Cursor exists. Don't reinvent it. Pinback's user is someone who wants
to *delegate*, not someone who wants to type code with assistance. If
they wanted Cursor they'd open Cursor.

### Per-workspace model selection

Falls out of the multi-agent argument. RAM-constrained, single-binary
v1. Defer.

### "Compose a prompt" assistants / prompt libraries

Building a prompt-library inside the GUI is a tar pit. The agent is
the agent. If a user has favorite prompts they paste them in. Adding
a "prompt manager" UI invites scope creep and is a vanity feature in
a product whose differentiator is "make working with ds4-agent
easier."

### Built-in chat with non-ds4 models, OpenAI/Anthropic adapters

Don't. The product is "the GUI for ds4-agent." Becoming a model-
agnostic chat client is a different product, with much more crowded
competition, and with a worse moat.

## Section 3 — The tar pit list

Things people will ask for that I'd flat-out resist unless someone
makes a strong case:

- **Voice input.** Not unless someone explicitly says they need it.
- **Themes, customization, extensibility frameworks, plugin systems.**
  Tar pit. Ship one good UI.
- **Sharing-as-a-service** (a hosted "share this conversation" link
  with auth, accounts, etc). The events.log already exports as a
  static HTML in one shot. That's enough.
- **Built-in evals harness, prompt regression suites, A/B model
  testing.** Different product.
- **Inline web rendering of agent-generated HTML/markdown previews
  with iframes.** Security tar pit. We have Shiki for code; that's
  the line.

## Section 4 — The constraint that shapes everything: `~/ds4` is read-only

Some of the highest-leverage features I listed need ds4-agent
cooperation to be done well:

- **Pending-changeset / accept-reject** wants a tool-I/O bridge so
  the agent's writes can be intercepted before they hit disk. Without
  it, pinback's options are: a fuse-mounted shadow filesystem (heavy
  on macOS, requires kernel ext), or post-hoc revert (works for
  edits, doesn't help with bash side effects).
- **Branching** wants a way to start a session from a saved KV with a
  modified history, not just resume.
- **Cost/usage telemetry** wants ds4-agent to expose the worker's
  prefill/decode tokens to a non-TUI surface.
- **Graceful turn-abort** wants ds4-agent to honor a stdin "/abort"
  command in non-interactive mode (today: SIGINT only, which kills
  the TUI's redraw cycle as a side effect).

For each of these, there are three honest options:

1. **Patch ds4-agent.** Off the table by your prior instruction.
2. **Run ds4-agent under a pty and emulate.** Doable for some, hacky
   for others. The pty pivot we discussed is the simplest case (just
   getting `/save` and `/switch` back). Tool-I/O interception is much
   harder over a pty.
3. **Defer the feature until upstream changes.** Honest. Document
   it. Don't fake it.

So a real product roadmap for pinback has to make peace with the
constraint by drawing a line: features that work without upstream
cooperation are first-class; features that need upstream go on a
"when ds4-agent grows hooks" list, openly.

## Section 5 — Product positioning, my actual take

The temptation is to position pinback as "Claude Code, but for ds4-
agent." That's wrong on two counts. First, it makes pinback a worse
copy of a more-resourced product. Second, it ignores ds4-agent's
actual differentiator, which is that it's a *local* coding agent
running on your hardware with your KV cache on your disk. Pinback
should lean into that.

Actual positioning I'd defend:

> Pinback is a remote-friendly cockpit for a local coding agent. The
> model lives on your laptop. The GUI lives on whatever you're
> looking at. Open `~/myproject` from your phone, send a refactor,
> close your phone. Come back to the laptop, the work is done, the
> diffs are reviewable.

This frames the four real wedges:

1. **Local-first agent, anywhere-access.** Tailscale + Pinback turns
   ds4-agent into a personal cloud agent without giving up local
   control.
2. **Review-then-apply.** Pinback's value isn't "chat better." It's
   "the agent did 50 things and now you review the diff." That's the
   delta over CLI.
3. **Single binary, no setup theatre.** noobs run one binary. No
   docker, no electron, no python venv. This is a real differentiator
   and we already have it.
4. **Honest workspace model.** "It's a folder you point at, not a
   repo you import." Better-than-Claude-Code for the kind of user
   who hops between many small projects.

Vanity features that don't serve those four wedges should be cut.

## Section 6 — What I'd actually build, in order

If you wanted me to write a real v1 plan after this exercise, the
order would be:

0. **Fix v0 — the pty pivot.** Without `/save` and `/switch`, "switch
   workspaces and pick up where you left off" doesn't work. Until
   that works, the workspace primitive is a lie. Start here. (The
   detail is in `docs/v0-postmortem.md`.)
1. **Inspectable tool-call cards.** Highest-leverage UX win. Cheap.
2. **Cross-workspace dashboard.** Makes pinback feel like a real
   product, not a chat box.
3. **Notification on `answer_end` when tab not focused.** Cheap.
   Real value.
4. **Long-run scrub timeline.** Builds on per-segment bubbles. Real
   value once any single conversation passes ~20 turns.
5. **Conversation export to single HTML file.** Free; events.log is
   already the data.
6. **Remote-access onboarding.** Tailscale Funnel + auth token.
   Cheap. Unlocks the positioning.
7. **Pending-changeset / accept-reject.** Highest-impact, highest-
   risk. This is where the upstream-cooperation conversation has to
   happen. Don't fake it.

Items 0–6 don't require any change to `~/ds4`. Item 7 does, in any
honest implementation. That's the natural breakpoint where pinback
either stays a thin cockpit or starts negotiating with upstream.

## Section 7 — Honest meta-take

The reason v0 broke is that I built to a plan instead of to the
source. The reason a v1 conversation could break the same way is if
we make a v1 plan that doesn't survive contact with `ds4_agent.c`.
Whatever we decide above, the next plan should have a "verified
against the source" column next to every item. If a row says
"requires ds4-agent to expose tool-I/O hook," we don't put it in v1
unless that hook actually exists or we're committed to landing it.

That isn't a process plea, it's the actual lesson. Everything else
in this document is opinion; that part is just procedure.
