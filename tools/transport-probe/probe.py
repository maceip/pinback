#!/usr/bin/env python3
"""
Day-0 transport probe for pinback <-> ds4-agent.

Goal: empirically settle whether pinback can drive ds4-agent's *interactive*
slash commands (/save, /switch) over PLAIN PIPES using LINENOISE_ASSUME_TTY,
without a pty. Captures exact bytes so the prose/turn-end/sha classifier can be
designed against reality instead of assumptions.

Two scenarios:
  tui   - spawn WITHOUT --non-interactive, env LINENOISE_ASSUME_TTY=1, pipes.
          Drive the full resume dance in one model load.
  nonint- spawn WITH --non-interactive, pipes. Confirm +DWARFSTAR_WAITING and
          that "/save" is treated as a model prompt (no sha) -> the v0 break.

Everything is captured raw to ./capture/ for later inspection.
"""
import os, sys, time, subprocess, threading, select, re, pathlib

DS4 = os.path.expanduser("~/ds4")
AGENT = os.path.join(DS4, "ds4-agent")
MODEL = os.path.join(DS4, "ds4flash.gguf")
CAP = pathlib.Path(__file__).resolve().parent / "capture"
CAP.mkdir(exist_ok=True)

SHA_RE = re.compile(rb"saved session ([0-9a-f]{6,40})")
SWITCH_RE = re.compile(rb"switched to session ([0-9a-f]{6,40})", re.I)
PROMPT = b"ds4-agent>"


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


class Proc:
    """Spawn ds4-agent over pipes; background-read stdout+stderr to buffers."""
    def __init__(self, extra_args, env_extra, tag):
        self.tag = tag
        env = dict(os.environ)
        env.update(env_extra)
        args = [AGENT, "--model", MODEL, "-c", "8192", "-n", "64",
                "--nothink", "--seed", "1"] + extra_args
        log(f"spawn[{tag}]: {' '.join(args)}  env+={env_extra}")
        self.p = subprocess.Popen(
            args, cwd=DS4, env=env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            bufsize=0)
        self.out = bytearray()
        self.err = bytearray()
        self.out_raw = open(CAP / f"{tag}.stdout.bin", "wb")
        self.err_raw = open(CAP / f"{tag}.stderr.bin", "wb")
        self._stop = False
        self._lock = threading.Lock()
        self._cpr_seen = 0      # how many ESC[6n we've answered
        self.cpr_replies = 0
        self._t = threading.Thread(target=self._pump, daemon=True)
        self._t.start()

    def _raw_send(self, b):
        with self._lock:
            self.p.stdin.write(b)
            self.p.stdin.flush()

    def _answer_cpr(self):
        # linenoise getColumns() blocks reading ESC[rows;colsR after each ESC[6n.
        # Emulate a wide terminal so it proceeds. One reply per request.
        total = bytes(self.out).count(b"\x1b[6n")
        while self._cpr_seen < total:
            self._cpr_seen += 1
            self.cpr_replies += 1
            self._raw_send(b"\x1b[1;200R")

    def _pump(self):
        fo, fe = self.p.stdout, self.p.stderr
        while not self._stop:
            r, _, _ = select.select([fo, fe], [], [], 0.1)
            for f in r:
                b = os.read(f.fileno(), 65536)
                if not b:
                    continue
                if f is fo:
                    self.out += b; self.out_raw.write(b); self.out_raw.flush()
                    self._answer_cpr()
                else:
                    self.err += b; self.err_raw.write(b); self.err_raw.flush()
            if self.p.poll() is not None and not r:
                break

    def send(self, s):
        log(f"send[{self.tag}]: {s!r}")
        self._raw_send(s if isinstance(s, bytes) else s.encode())

    def wait_for(self, needle, where="out", timeout=600, after=0):
        """Wait until `needle` appears in the buffer at/after byte offset `after`."""
        needle = needle if isinstance(needle, bytes) else needle.encode()
        deadline = time.time() + timeout
        while time.time() < deadline:
            buf = self.out if where == "out" else self.err
            idx = bytes(buf).find(needle, after)
            if idx >= 0:
                return idx
            if self.p.poll() is not None:
                time.sleep(0.2)
                buf = self.out if where == "out" else self.err
                idx = bytes(buf).find(needle, after)
                return idx if idx >= 0 else -1
            time.sleep(0.15)
        return -1

    def olen(self):
        return len(self.out)

    def close(self):
        self._stop = True
        try:
            self.p.terminate()
            self.p.wait(timeout=5)
        except Exception:
            try: self.p.kill()
            except Exception: pass
        self.out_raw.close(); self.err_raw.close()


def show(label, b):
    print(f"\n----- {label} (repr) -----")
    print(repr(bytes(b))[:1600])
    print(f"----- {label} (decoded, escapes stripped) -----")
    txt = re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", bytes(b))
    txt = txt.replace(b"\r", b"\n")
    sys.stdout.buffer.write(txt[:1600]); sys.stdout.flush()
    print()


def scenario_tui():
    log("=== SCENARIO: TUI over pipes via LINENOISE_ASSUME_TTY ===")
    pr = Proc(extra_args=[], env_extra={"LINENOISE_ASSUME_TTY": "1", "TERM": "dumb"},
              tag="tui")
    try:
        # 1. boot -> first prompt
        idx = pr.wait_for(PROMPT, "out", timeout=600)
        log(f"boot prompt at out offset {idx} (elapsed includes model load)")
        if idx < 0:
            log("NO PROMPT after boot -> capturing what we got")
            show("tui boot stdout", pr.out[:2000]); show("tui boot stderr", pr.err[:2000])
            return
        show("tui after-boot stdout", pr.out)

        # 2. one tiny turn to make the session dirty
        mark = pr.olen()
        pr.send("reply with just the word ok\r")
        # turn end = prompt reappears after our input
        end = pr.wait_for(PROMPT, "out", timeout=300, after=mark + 5)
        log(f"turn-1 end (prompt reappears) at {end}")
        show("tui turn-1 output", pr.out[mark:])

        # 3. /save -> expect a sha
        mark = pr.olen()
        pr.send("/save\r")
        time.sleep(2.0)
        pr.wait_for(PROMPT, "out", timeout=60, after=mark + 5)
        seg = bytes(pr.out[mark:])
        show("tui /save output", seg)
        m = SHA_RE.search(seg)
        sha = m.group(1).decode() if m else None
        log(f"/save SHA parsed: {sha!r}   (regex 'saved session <hex>')")

        # 4. /list
        mark = pr.olen()
        pr.send("/list\r")
        time.sleep(1.5); pr.wait_for(PROMPT, "out", timeout=30, after=mark + 5)
        show("tui /list output", pr.out[mark:])

        # 5. /new then /switch <sha> -> expect history restored
        if sha:
            pr.send("/new\r"); time.sleep(1.0); pr.wait_for(PROMPT, "out", 30, pr.olen()-1)
            mark = pr.olen()
            pr.send(f"/switch {sha}\r")
            time.sleep(2.0); pr.wait_for(PROMPT, "out", 120, after=mark + 5)
            seg = bytes(pr.out[mark:])
            show("tui /switch output", seg)
            sm = SWITCH_RE.search(seg)
            log(f"/switch ack parsed: {sm.group(1).decode() if sm else None!r}; "
                f"history-restored evidence: {b'ok' in seg.lower() or b'recent' in seg.lower()}")
        pr.send("/quit\r")
        time.sleep(1.0)
    finally:
        pr.close()
        log("=== TUI verdict ===")
        log(f"  saved-session SHA captured over PIPES (no pty): "
            f"{'YES' if SHA_RE.search(bytes(pr.out)) else 'NO'}")


def scenario_nonint():
    log("=== SCENARIO: --non-interactive baseline ===")
    pr = Proc(extra_args=["--non-interactive"], env_extra={}, tag="nonint")
    try:
        idx = pr.wait_for(b"+DWARFSTAR_WAITING", "err", timeout=600)
        log(f"+DWARFSTAR_WAITING on stderr at {idx} -> {'FOUND' if idx>=0 else 'MISSING'}")
        mark = pr.olen()
        pr.send("/save\r")
        # wait for next WAITING marker = turn done
        recount = bytes(pr.err).count(b"+DWARFSTAR_WAITING")
        t0 = time.time()
        while time.time() - t0 < 120:
            if bytes(pr.err).count(b"+DWARFSTAR_WAITING") > recount:
                break
            time.sleep(0.2)
        seg = bytes(pr.out[mark:])
        show("nonint '/save' output (should be model prose, NO sha)", seg)
        log(f"  sha present? {'YES (unexpected)' if SHA_RE.search(seg) else 'NO -> confirms v0 break'}")
    finally:
        pr.close()


if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "tui"
    if which in ("tui", "all"):
        scenario_tui()
    if which in ("nonint", "all"):
        scenario_nonint()
    log("done; raw captures in ./capture/")
