#!/usr/bin/env python3
"""
Reference (spec) implementation of pinback's VT content extractor.

This is the EXACT algorithm to port to src/vterm.c. It models the small
terminal screen ds4-agent draws in LINENOISE_ASSUME_TTY mode and recovers the
clean content lines (model answer + DSML + tool_results), dropping the editor
widget (prompt row + status footer) and the cursor/redraw escapes.

Validated against capture/stream.raw.bin -> the haiku + one/two/three, with the
pyte emulator as an independent oracle (vterm_oracle_check.py).

Escape subset handled (everything ds4-agent's editor actually emits):
  BS, CR, LF, ESC[<n>A (up), ESC[<n>B (down), ESC[<n>C (fwd), ESC[<n>D (back),
  ESC[<n>G (column), ESC[<n>K / ESC[K (erase line; n=0 to EOL, 1 to BOL, 2 all),
  ESC[?<n>l / ESC[?<n>h (mode set/reset, ignored), ESC[<...>m (SGR, ignored),
  ESC[6n (CPR request -> callback), ESC[<n>;<m>R (CPR reply from us, ignored).
Unknown CSI is consumed and ignored. Cursor never leaves the grid.
"""

COLS = 220
ROWS = 24
PROMPT = "ds4-agent>"


class VTerm:
    def __init__(self, on_cpr=None):
        self.cols = COLS
        self.rows = ROWS
        self.grid = [[" "] * self.cols for _ in range(self.rows)]
        self.cr = 0          # cursor row
        self.cc = 0          # cursor col
        self.scrollback = []  # finalized rows pushed off the top
        self.on_cpr = on_cpr
        self._esc = b""       # in-progress escape sequence (after ESC)

    # ---- screen primitives ----
    def _scroll_up(self):
        self.scrollback.append("".join(self.grid[0]).rstrip())
        self.grid.pop(0)
        self.grid.append([" "] * self.cols)

    def _newline(self):
        if self.cr == self.rows - 1:
            self._scroll_up()
        else:
            self.cr += 1

    def _putc(self, ch):
        if self.cc >= self.cols:
            # ds4-agent disables autowrap (?7l) around the status line and
            # generally keeps content within width; clamp rather than wrap.
            self.cc = self.cols - 1
        self.grid[self.cr][self.cc] = ch
        self.cc += 1

    # ---- escape handling ----
    def _csi(self, body):
        # body is the bytes between ESC[ and the final letter, plus final.
        final = body[-1:]
        params = body[:-1]
        priv = params.startswith(b"?")
        if priv:
            params = params[1:]
        nums = []
        for part in params.split(b";"):
            try:
                nums.append(int(part)) if part else nums.append(0)
            except ValueError:
                nums.append(0)
        n = nums[0] if nums else 0
        f = final
        if priv:
            return  # ?7l / ?7h / ?2004h etc: modes we ignore
        if f == b"A":
            self.cr = max(0, self.cr - max(1, n))
        elif f == b"B":
            self.cr = min(self.rows - 1, self.cr + max(1, n))
        elif f == b"C":
            self.cc = min(self.cols - 1, self.cc + max(1, n))
        elif f == b"D":
            self.cc = max(0, self.cc - max(1, n))
        elif f == b"G":
            self.cc = min(self.cols - 1, max(0, n - 1))
        elif f in (b"H", b"f"):
            row = (nums[0] if len(nums) >= 1 else 1) or 1
            col = (nums[1] if len(nums) >= 2 else 1) or 1
            self.cr = min(self.rows - 1, max(0, row - 1))
            self.cc = min(self.cols - 1, max(0, col - 1))
        elif f == b"K":
            if n == 0:
                for x in range(self.cc, self.cols):
                    self.grid[self.cr][x] = " "
            elif n == 1:
                for x in range(0, self.cc + 1):
                    self.grid[self.cr][x] = " "
            elif n == 2:
                self.grid[self.cr] = [" "] * self.cols
        elif f == b"J":
            # erase display: only handle n=2 (all) conservatively
            if n == 2:
                for r in range(self.rows):
                    self.grid[r] = [" "] * self.cols
                self.cr = self.cc = 0
        elif f == b"n":
            if n == 6 and self.on_cpr:
                self.on_cpr()
        elif f == b"m":
            pass  # SGR colors: ignore
        else:
            pass  # unknown CSI: ignore

    def feed(self, data: bytes):
        # _esc states: b"" = normal; b"\x1b" = saw ESC; b"\x1b[" + body = in CSI
        for o in data:
            b = bytes((o,))
            if self._esc == b"\x1b":
                # byte right after ESC
                if b == b"[":
                    self._esc = b"\x1b["
                else:
                    self._esc = b""   # ESC + non-[ (ESC c, ESC M, ...): ignore
                continue
            if self._esc.startswith(b"\x1b["):
                # in CSI body; final byte is 0x40..0x7E AFTER the '['
                self._esc += b
                if 0x40 <= o <= 0x7E:
                    self._csi(self._esc[2:])
                    self._esc = b""
                continue
            if o == 0x1B:       # ESC
                self._esc = b"\x1b"
            elif o == 0x0D:     # CR
                self.cc = 0
            elif o == 0x0A:     # LF
                self._newline()
            elif o == 0x08:     # BS
                self.cc = max(0, self.cc - 1)
            elif o == 0x09:     # TAB
                self.cc = min(self.cols - 1, (self.cc // 8 + 1) * 8)
            elif o >= 0x20:     # printable (incl utf-8 continuation bytes)
                self._putc(b.decode("latin-1"))

    # ---- content extraction ----
    def prompt_row(self):
        """Index of the editor widget's top (the prompt row), or rows if none."""
        for r in range(self.rows - 1, -1, -1):
            if "".join(self.grid[r]).startswith(PROMPT):
                return r
        return self.rows

    def status_state(self):
        """Current footer phase: 'idle' | 'reading' | 'generation' | None."""
        for r in range(self.rows - 1, -1, -1):
            line = "".join(self.grid[r]).rstrip()
            k = line.find("| ")
            if "ctx " in line and k >= 0:
                tail = line[k + 2:].strip()
                return tail.split()[0] if tail else None
        return None

    def content_rows(self):
        """All finalized content above the editor widget (scrollback + grid)."""
        rows = list(self.scrollback)
        pr = self.prompt_row()
        for r in range(pr):
            rows.append("".join(self.grid[r]).rstrip())
        return rows


if __name__ == "__main__":
    import sys, re
    data = open("capture/stream.raw.bin", "rb").read()
    vt = VTerm(on_cpr=lambda: None)
    vt.feed(data)
    rows = vt.content_rows()
    # drop the user-echo line ("* ...") and leading/trailing blanks for display
    body = [r for r in rows if not r.startswith("* ")]
    while body and not body[0].strip():
        body.pop(0)
    while body and not body[-1].strip():
        body.pop()
    text = "\n".join(body)
    print("=== extracted content ===")
    print(text)
    # correctness assertions (the model's actual answer, line structure intact)
    haiku = "A vast blue expanse\nWaves whisper to ancient shores\nDepths hold silent dreams"
    count = "one\ntwo\nthree"
    ok = (haiku in text) and (count in text)
    noise = any(s in text for s in ("ds4-agent>", "ctx ", "reading [", "generation "))
    print("\n=== checks ===")
    print("haiku lines intact:", haiku in text)
    print("count lines intact:", count in text)
    print("no widget noise   :", not noise)
    sys.exit(0 if (ok and not noise) else 1)
