# ds4-agent transport — empirical findings (LOCKED)

Status: **verified against the real `ds4-agent` binary** on 2026-06-04, not
inferred from a plan. Every claim here has a captured byte-stream behind it
under `tools/transport-probe/capture/`. If you are tempted to re-litigate the
transport (server-vs-agent, pipe-vs-pty, which mode), read this first and re-run
the probe instead of restarting the project.

## TL;DR

- pinback drives `ds4-agent` over **plain pipes. No pty.**
- The feature "switch dirs and pick up where you left off" needs the real
  `/save` + `/switch` commands, which only work in ds4-agent's **interactive
  (TUI) loop**, not in `--non-interactive`.
- The TUI loop runs over pipes when you set **`LINENOISE_ASSUME_TTY=1`**.
- The cost is that the TUI emits linenoise cursor/redraw escapes; pinback
  reconstructs clean content with a small virtual-terminal model (`src/vterm.c`),
  verified against the `pyte` emulator as an oracle.

## The two modes (chosen by the `--non-interactive` flag, `ds4_agent.c:9686`)

| | `--non-interactive` | TUI (`LINENOISE_ASSUME_TTY=1`, no flag) |
|---|---|---|
| stdin | one line = one prompt | linenoise editor (submit with **CR `\r`**, not `\n`) |
| stdout | **clean** prose+DSML (`ok\n\n`, zero escapes) | prose+DSML wrapped in editor redraws |
| `/save` `/switch` | **ignored** — model just hallucinates ("I've saved…") | **work** — real KV save/restore |
| turn-end | `+DWARFSTAR_WAITING` on stderr | status footer state returns to `idle` |
| pty | not needed | not needed |

Mode selection is the **flag**, not `isatty()`. The `isatty` checks at
`ds4_agent.c:8346/8554` only gate features *inside* the TUI.

## Why the TUI needs LINENOISE_ASSUME_TTY (not just dropping the flag)

`enableRawMode()` (`linenoise.c:574`) skips terminal setup only when
`LINENOISE_ASSUME_TTY` is set; otherwise on a pipe it hits `goto fatal` and
`linenoiseEditStart` returns -1 (`linenoise.c:1951`) — the editor never starts.
So the env var is **mandatory** for TUI-over-pipes. There is no clean no-env path.

## The verified driver recipe (what `src/agent.c` must do)

1. Spawn `ds4-agent --chdir <ws> --model <m>` — **drop `--non-interactive`**.
2. Child env: `LINENOISE_ASSUME_TTY=1`, `TERM=dumb`, plus the existing
   `DS4_METAL_*_SOURCE` absolute paths. (Note: metal sources resolve relative to
   **cwd**, not `argv[0]` — the old ARCHITECTURE_REDO.md claim was wrong, though
   the absolute-path workaround is correct either way.)
3. Three pipes (stdin/stdout/stderr) — same as before, no pty.
4. **CPR:** whenever stdout contains `ESC[6n`, write `ESC[1;200R` to stdin.
   linenoise's `getColumns()` (`linenoise.c:654`) blocks on this at boot and at
   every editor restart. Without the reply the agent hangs before the prompt.
   (~2 at boot, then 1 per turn.)
5. **Submit** every prompt and slash command with a trailing **`\r`** (CR).
   linenoise Enter is `ENTER=13` (`linenoise.c:500`); `\n` is swallowed as a
   literal char and the line never submits.

## stdout byte grammar (what the filter parses)

- Idle prompt row: `ds4-agent> ` (after `\r\x1b[0K`).
- Status footer: bracketed by `\x1b[?7l` … `\x1b[?7h`, content
  `ctx <used>/<total> | <state>` where `<state>` ∈
  `idle` | `reading [<bar>] N/M %` | `generation N tokens X t/s`.
  **Turn-end = state returns to `idle` while we are BUSY.**
- User-turn echo: a content row `* <the prompt>` (drop it — pinback emits the
  `user` event itself).
- Per generated token (during `generation`):
  `\x1b[1A\x1b[<col>G<TOKEN>\r\n` then a prompt+status redraw. Tokens are written
  to the content line above the prompt by **absolute column** — which is why a
  naive strip collapses lines and you need a cursor model.
- Command acks (parse these): `saved session <8hex>`,
  `switched to session <8hex> (<N> tokens)`, plus a
  `--- session history: last K user turns --- … --- end history ---` block.
- `/quit` prints `save scheduled at next safe point`.
- Saved sessions live in `~/.ds4/kvcache/<full-sha>.kv`; the 8-char prefix is
  accepted by `/switch`.

## Tool calls are pretty-rendered, NOT raw DSML (BOTH modes)

The single most important correction to the v0 design. The model *generates*
raw DSML (`<｜DSML｜tool_calls>…`, per the system prompt at `ds4_agent.c:658`),
but ds4-agent's stream renderer (`agent_tool_viz_*`, `ds4_agent.c:2638`)
**consumes the DSML and emits a compact human visualization** instead:

```
🛠️ write  path=/tmp/pinback_probe2.txt
banana
```

Verified in BOTH `--non-interactive` and TUI mode — neither puts raw
`<｜DSML｜tool_calls>` on stdout (`capture/nonint_dsml.raw.bin`,
`capture/dsml.raw.bin`; the fullwidth `｜` byte 0xEF 0xBD 0x9C never appears).
There is no flag to disable the pretty rendering.

Consequences:
- pinback's v0 DSML classifier (and the DSML-emitting fake agent) match output
  the real agent never produces — in any mode. That code is dead against reality.
- For v0, display the clean stdout as-is: prose plus `🛠️ <tool> <params>` lines,
  styled lightly as tool activity. Novice-friendly already.
- Structured tool data (exact old/new for a diff viewer) is recoverable only
  from the model's raw tokens via **`--trace FILE`** (`ds4_agent.c:1128`,
  per-token `hex=` field, flushed per token). Defer to the diff-viewer milestone;
  it works in either transport mode.

## The filter (src/vterm.c) — deferred with the TUI/fast-resume path

A bounded virtual-terminal screen (grid + scrollback) that honors
CR/LF/BS/TAB, `ESC[` A/B/C/D (cursor), G (column), H/f (position), K/J (erase),
m (SGR, ignored), `?`-private modes (ignored), and `6n` (CPR → callback).
Content = all rows above the editor widget (prompt row + status), in order, with
scrollback. The reference algorithm is `tools/transport-probe/vterm_ref.py`,
validated to reconstruct the captured haiku + count with correct line breaks and
zero widget noise, cross-checked against `pyte`.

## Reproduce / regression

```
cd tools/transport-probe
python3 probe.py tui        # full /save -> /switch dance over pipes
python3 vterm_ref.py        # filter reconstructs clean content from the fixture
```

Captures retained: `stream.raw.bin` (multi-line generation), `tui.stdout.bin`
(the save/switch dance), `dsml.raw.bin` (a real tool-call turn).
