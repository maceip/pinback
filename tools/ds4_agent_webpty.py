#!/usr/bin/env python3
"""Tiny authenticated web PTY for the local DS4 native agent."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import mimetypes
import os
import pty
import re
import secrets
import selectors
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import termios
import threading
import time
import urllib.parse
from collections import deque
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


ANSI_RE = re.compile(r"\x1b\][^\x07]*(?:\x07|\x1b\\)|\x1b\[[0-?]*[ -/]*[@-~]|\x1b[ -/]*[@-~]")
TOKEN_SKIP_PREFIXES = ("ds4:", "ds4-agent:", "+DWARFSTAR", "done", "Usage:")
SCRIPT_DIR = Path(__file__).resolve().parent
UI_DIST = SCRIPT_DIR / "ds4-agent-ui" / "dist"
UI_DIST_ROOT = UI_DIST.resolve()
DS4_CONTRACT_PATH = SCRIPT_DIR / "ds4-agent-ui" / "ds4-interface-contract.json"
RUNTIME_DIR = Path.home() / ".ds4" / "runtime"
ASSET_PATHS = {
    "/asset/bg-dark.jpg": ("tools/assets/bg-dark.jpg", "image/jpeg"),
    "/asset/bg-light.jpg": ("tools/assets/bg-light.jpg", "image/jpeg"),
    "/asset/icon-sheet.jpg": ("tools/assets/icon-sheet.jpg", "image/jpeg"),
    "/asset/avatar-agent.jpg": ("tools/assets/avatar-agent.jpg", "image/jpeg"),
    "/asset/avatar-agent-backup.jpg": ("tools/assets/avatar-agent-backup.jpg", "image/jpeg"),
    "/asset/avatar-user.jpg": ("tools/assets/avatar-user.jpg", "image/jpeg"),
    "/asset/panels-wide.jpg": ("tools/assets/panels-wide.jpg", "image/jpeg"),
    "/asset/panel-large.jpg": ("tools/assets/panel-large.jpg", "image/jpeg"),
    "/asset/app-icon.jpg": ("tools/assets/app-icon.jpg", "image/jpeg"),
    "/asset/app-wordmark.jpg": ("tools/assets/app-wordmark.jpg", "image/jpeg"),
    "/asset/app-rmark.jpg": ("tools/assets/app-rmark.jpg", "image/jpeg"),
    "/asset/offline.jpg": ("tools/assets/offline.jpg", "image/jpeg"),
    "/asset/engine-cube.jpg": ("tools/assets/engine-cube.jpg", "image/jpeg"),
    "/asset/pattern-filler.jpg": ("tools/assets/pattern-filler.jpg", "image/jpeg"),
}


class PreflightError(RuntimeError):
    pass


class RuntimeLock:
    def __init__(self, path: Path):
        self.path = path
        self.fd: int | None = None

    def acquire(self) -> bool:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(self.path, os.O_CREAT | os.O_RDWR, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            os.close(fd)
            return False
        self.fd = fd
        os.ftruncate(fd, 0)
        os.write(fd, str(os.getpid()).encode("ascii"))
        return True

    def release(self) -> None:
        if self.fd is None:
            return
        try:
            fcntl.flock(self.fd, fcntl.LOCK_UN)
        finally:
            os.close(self.fd)
            self.fd = None


def profile_kind() -> str:
    return "agent-local-metal" if sys.platform == "darwin" else "agent-linux-cuda"


def profile_id(kind: str, ds4_root: Path, port: int) -> str:
    digest = hashlib.sha1(f"{kind}:{ds4_root}:{port}".encode("utf-8")).hexdigest()[:12]
    return f"{kind}-{digest}"


def runtime_paths(runtime_dir: Path, ident: str) -> tuple[Path, Path]:
    safe = re.sub(r"[^A-Za-z0-9_.-]+", "-", ident)
    return runtime_dir / f"{safe}.json", runtime_dir / f"{safe}.lock"


def pid_alive(pid: int | None) -> bool:
    if not pid or pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def load_runtime_state(path: Path) -> dict[str, object] | None:
    try:
        payload = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return None
    return payload if isinstance(payload, dict) else None


def atomic_write_json(path: Path, payload: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    os.replace(tmp, path)


def clear_runtime_state(path: Path) -> None:
    payload = load_runtime_state(path)
    if payload:
        try:
            bridge_pid = int(payload.get("bridge_pid", 0))
        except (TypeError, ValueError):
            bridge_pid = 0
        if bridge_pid not in (0, os.getpid()) and pid_alive(bridge_pid):
            return
    try:
        path.unlink()
    except FileNotFoundError:
        pass


def listener_pids(port: int) -> list[int]:
    lsof = shutil.which("lsof")
    if not lsof:
        return []
    try:
        output = subprocess.check_output(
            [lsof, "-nP", "-tiTCP:%d" % port, "-sTCP:LISTEN"],
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except subprocess.CalledProcessError:
        return []
    except OSError:
        return []
    pids: list[int] = []
    for line in output.splitlines():
        try:
            pids.append(int(line.strip()))
        except ValueError:
            continue
    return sorted(set(pids))


def process_command(pid: int) -> str:
    try:
        return subprocess.check_output(["/bin/ps", "-p", str(pid), "-o", "command="], text=True).strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def ensure_executable(command: list[str], cwd: str) -> None:
    if not command:
        raise PreflightError("no DS4 command was provided")
    program = command[0]
    candidate = Path(program)
    if candidate.is_absolute() or "/" in program:
        resolved = candidate if candidate.is_absolute() else Path(cwd) / candidate
        if not resolved.exists():
            raise PreflightError(f"DS4 command does not exist: {resolved}")
        if not os.access(resolved, os.X_OK):
            raise PreflightError(f"DS4 command is not executable: {resolved}")
        return
    if not shutil.which(program):
        raise PreflightError(f"DS4 command is not on PATH: {program}")


def ensure_model_exists(command: list[str], cwd: str) -> None:
    model = command_option(command, "--model", "-m")
    if not model:
        return
    path = Path(model)
    if not path.is_absolute():
        path = Path(cwd) / path
    if not path.exists():
        raise PreflightError(f"model file does not exist: {path}")


def ensure_contract_current(ds4_root: Path, allow_drift: bool) -> None:
    contract = load_contract()
    if not isinstance(contract, dict):
        raise PreflightError(f"DS4 contract file is missing or invalid: {DS4_CONTRACT_PATH}")
    ds4_head = git_head(ds4_root)
    contract_head = contract.get("ds4_revision")
    if not ds4_head:
        raise PreflightError(f"cannot resolve DS4 git head in {ds4_root}")
    if ds4_head != contract_head and not allow_drift:
        raise PreflightError(
            f"DS4 contract drift: bridge contract {contract_head or 'unknown'} does not match {ds4_head}"
        )


def preflight_runtime(
    *,
    ds4_root: Path,
    command: list[str],
    cwd: str,
    port: int,
    allow_contract_drift: bool,
) -> None:
    if not ds4_root.is_dir():
        raise PreflightError(f"DS4 root does not exist: {ds4_root}")
    if not (ds4_root / ".git").exists():
        raise PreflightError(f"DS4 root is not a git checkout: {ds4_root}")
    ensure_executable(command, cwd)
    ensure_model_exists(command, cwd)
    ensure_contract_current(ds4_root, allow_contract_drift)
    pids = listener_pids(port)
    if pids:
        details = "; ".join(f"{pid} {process_command(pid)}".strip() for pid in pids)
        raise PreflightError(f"port {port} is already listening; owner: {details or pids}")


def print_existing_runtime(payload: dict[str, object], token: str | None = None) -> None:
    bridge = payload.get("bridge") if isinstance(payload.get("bridge"), dict) else {}
    host = str(bridge.get("host", "127.0.0.1")) if isinstance(bridge, dict) else "127.0.0.1"
    port = int(bridge.get("port", 0)) if isinstance(bridge, dict) and bridge.get("port") else 0
    token_hash = payload.get("token_sha256")
    existing_token = token if token and hashlib.sha256(token.encode("utf-8")).hexdigest() == token_hash else ""
    print("DS4 web PTY already running from owned runtime profile:")
    print(f"  bridge pid: {payload.get('bridge_pid')}")
    print(f"  agent pid: {payload.get('agent_pid')}")
    if port:
        suffix = f"?token={urllib.parse.quote(existing_token)}" if existing_token else ""
        print(f"  local: http://127.0.0.1:{port}/{suffix}")
        if host not in ("127.0.0.1", "localhost"):
            print(f"  phone: http://{host}:{port}/{suffix}")
    sys.stdout.flush()


MISSING_UI_HTML = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>DS4 Agent UI build missing</title>
  <style>
    :root { color-scheme: dark; }
    body { margin: 0; min-height: 100vh; display: grid; place-items: center; background: #0b0d10; color: #f4f7fb; font: 15px/1.45 system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; }
    main { width: min(34rem, calc(100vw - 2rem)); border: 1px solid rgba(255,255,255,.18); border-radius: 14px; background: rgba(18,20,25,.92); padding: 1rem; box-shadow: 0 20px 60px rgba(0,0,0,.38); }
    h1 { margin: 0 0 .5rem; font-size: 1rem; }
    p { margin: .35rem 0; color: #b5bfcc; }
    code { color: #b7ff2a; }
  </style>
</head>
<body>
  <main>
    <h1>DS4 Agent UI build missing</h1>
    <p>The bridge is running, but the React cockpit bundle is not present.</p>
    <p>Build it with <code>cd tools/ds4-agent-ui && bun run build</code>, then reload.</p>
  </main>
</body>
</html>"""


LOGIN_HTML = r"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>DS4 Agent Login</title>
  <style>
    color-scheme: dark;
    body {
      min-height: 100vh;
      margin: 0;
      display: grid;
      place-items: center;
      background: #101214;
      color: #e6edf3;
      font: 15px/1.4 system-ui, -apple-system, BlinkMacSystemFont, sans-serif;
    }
    form {
      width: min(92vw, 420px);
      display: grid;
      gap: 12px;
      padding: 20px;
      border: 1px solid #30363d;
      border-radius: 8px;
      background: #181b1f;
    }
    input, button {
      min-height: 44px;
      border: 1px solid #30363d;
      border-radius: 6px;
      padding: 0 12px;
      font: inherit;
    }
    input {
      background: #0d1117;
      color: #e6edf3;
    }
    button {
      background: #238636;
      color: white;
      border-color: #2ea043;
    }
  </style>
</head>
<body>
  <form method="post" action="/login">
    <strong>DS4 Agent</strong>
    <label for="token">Access token</label>
    <input id="token" name="token" type="password" autofocus>
    <button type="submit">Open</button>
  </form>
</body>
</html>
"""


class PtySession:
    def __init__(self, command: list[str], cwd: str, env: dict[str, str]):
        self.command = command
        self.cwd = cwd
        self.env = env
        self.fd = -1
        self.pid = -1
        self.seq = 0
        self.started_at = time.time()
        self.token_total = 0
        self.token_samples: deque[tuple[float, int]] = deque(maxlen=4096)
        self.turns = 0
        self.waiting = False
        self.exit_status: int | None = None
        self.runtime: dict[str, object] = {}
        self.chunks: deque[tuple[int, str]] = deque(maxlen=3000)
        self.cond = threading.Condition()
        self.lifecycle_lock = threading.Lock()
        self.generation = 0

    def start(self) -> None:
        with self.lifecycle_lock:
            self._start_locked()

    def _start_locked(self) -> None:
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(self.cwd)
            os.environ.update(self.env)
            os.execvpe(self.command[0], self.command, os.environ)
        self.generation += 1
        self.pid = pid
        self.fd = fd
        self.started_at = time.time()
        self.waiting = False
        self.exit_status = None
        self.runtime = {}
        self._set_window(38, 110)
        self._disable_terminal_echo(fd)
        os.set_blocking(fd, False)
        threading.Thread(target=self._reader, args=(pid, fd, self.generation), daemon=True).start()

    def _disable_terminal_echo(self, fd: int) -> None:
        try:
            attrs = termios.tcgetattr(fd)
            attrs[3] = attrs[3] & ~termios.ECHO
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
        except OSError:
            pass

    def _set_window(self, rows: int, cols: int) -> None:
        winsz = struct.pack("HHHH", rows, cols, 0, 0)
        try:
            import fcntl

            fcntl.ioctl(self.fd, termios.TIOCSWINSZ, winsz)
        except OSError:
            pass

    def _append(self, text: str) -> None:
        with self.cond:
            self.seq += 1
            self.chunks.append((self.seq, text))
            self._record_metrics_locked(text)
            self.cond.notify_all()

    def _record_metrics_locked(self, text: str) -> None:
        cleaned = ANSI_RE.sub("", text).replace("\r", "\n")
        if "+DWARFSTAR_WAITING" in cleaned:
            self.waiting = True
        ctx = re.search(
            r"context buffers [^(]*\(ctx=(\d+), backend=([^,\s]+), prefill_chunk=(\d+)",
            cleaned,
        )
        if ctx:
            self.runtime.update(
                {
                    "ctx": int(ctx.group(1)),
                    "backend": ctx.group(2),
                    "prefill_chunk": int(ctx.group(3)),
                }
            )
        pieces = 0
        for raw_line in cleaned.splitlines() or [cleaned]:
            line = raw_line.strip()
            if not line:
                continue
            if line.startswith(TOKEN_SKIP_PREFIXES):
                continue
            pieces += max(1, len(re.findall(r"\S+", line)))
        if pieces:
            self.token_total += pieces
            self.token_samples.append((time.time(), pieces))

    def _reader(self, pid: int, fd: int, generation: int) -> None:
        selector = selectors.DefaultSelector()
        selector.register(fd, selectors.EVENT_READ)
        while True:
            events = selector.select(timeout=0.25)
            if events:
                try:
                    data = os.read(fd, 8192)
                except BlockingIOError:
                    continue
                except OSError:
                    break
                if not data:
                    break
                self._append(data.decode("utf-8", errors="replace"))
            try:
                done, status = os.waitpid(pid, os.WNOHANG)
            except ChildProcessError:
                break
            if done == pid:
                with self.cond:
                    if generation == self.generation:
                        self.exit_status = status
                break
        with self.cond:
            self.cond.notify_all()

    def write(self, text: str) -> None:
        if self.exit_status is not None:
            raise RuntimeError("agent process has exited")
        if text.strip() and text != "\x03":
            with self.cond:
                self.turns += 1
                self.waiting = False
        try:
            os.write(self.fd, text.encode("utf-8", errors="replace"))
        except OSError as exc:
            with self.cond:
                self.exit_status = self.exit_status if self.exit_status is not None else -1
                self.waiting = False
                self.cond.notify_all()
            raise RuntimeError("agent PTY is no longer accepting input") from exc

    def wait_until_ready(self, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                if self.waiting and self.exit_status is None:
                    return True
                if self.exit_status is not None:
                    return False
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False
                self.cond.wait(timeout=remaining)

    def metrics(self) -> dict[str, object]:
        now = time.time()
        with self.cond:
            recent = [(ts, count) for ts, count in self.token_samples if now - ts <= 15]
            recent_count = sum(count for _, count in recent)
            span = max(1.0, min(15.0, now - recent[0][0])) if recent else 1.0
            return {
                "pid": self.pid,
                "seq": self.seq,
                "turns": self.turns,
                "uptime_sec": round(now - self.started_at, 1),
                "waiting": self.waiting,
                "exited": self.exit_status is not None,
                "exit_status": self.exit_status,
                "tokens": {
                    "total": self.token_total,
                    "per_second": recent_count / span,
                    "window_sec": 15,
                },
                "runtime": dict(self.runtime),
            }

    def terminate(self) -> None:
        with self.lifecycle_lock:
            self._terminate_locked(signal.SIGTERM)

    def _terminate_locked(self, sig: int) -> None:
        if self.pid > 0 and self.exit_status is None:
            try:
                os.kill(self.pid, sig)
            except ProcessLookupError:
                pass

    def restart(self) -> None:
        with self.lifecycle_lock:
            old_pid = self.pid
            old_fd = self.fd
            old_live = old_pid > 0 and self.exit_status is None
            if old_live:
                try:
                    os.kill(old_pid, signal.SIGTERM)
                except ProcessLookupError:
                    old_live = False
            if old_fd >= 0:
                try:
                    os.close(old_fd)
                except OSError:
                    pass
            if old_live:
                deadline = time.time() + 8
                while time.time() < deadline:
                    try:
                        done, _status = os.waitpid(old_pid, os.WNOHANG)
                    except ChildProcessError:
                        break
                    if done == old_pid:
                        break
                    time.sleep(0.1)
                else:
                    try:
                        os.kill(old_pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
            with self.cond:
                self.exit_status = None
                self.waiting = False
                self.cond.notify_all()
            self._start_locked()
        self._append("\n[bridge] ds4-agent restarted from owned runtime profile\n")


class State:
    def __init__(
        self,
        token: str,
        session: PtySession,
        ds4_root: Path,
        host: str,
        port: int,
        ident: str,
        state_file: Path,
        ready_timeout: float,
    ):
        self.token = token
        self.session = session
        self.ds4_root = ds4_root
        self.host = host
        self.port = port
        self.profile_kind = profile_kind()
        self.profile_id = ident
        self.state_file = state_file
        self.ready_timeout = ready_timeout
        self.ownership = "owned"


def parse_cookies(header: str | None) -> dict[str, str]:
    cookies: dict[str, str] = {}
    if not header:
        return cookies
    for part in header.split(";"):
        if "=" in part:
            key, value = part.split("=", 1)
            cookies[key.strip()] = urllib.parse.unquote(value.strip())
    return cookies


def mac_memory(agent_pid: int) -> dict[str, float | None]:
    out: dict[str, float | None] = {
        "total_mb": None,
        "used_mb": None,
        "used_pct": None,
        "agent_rss_mb": None,
    }
    try:
        total_bytes = int(subprocess.check_output(["/usr/sbin/sysctl", "-n", "hw.memsize"], text=True).strip())
        vm_text = subprocess.check_output(["/usr/bin/vm_stat"], text=True)
        page_size_match = re.search(r"page size of (\d+) bytes", vm_text)
        page_size = int(page_size_match.group(1)) if page_size_match else 16384
        pages: dict[str, int] = {}
        for line in vm_text.splitlines():
            if ":" not in line:
                continue
            key, raw = line.split(":", 1)
            value = re.sub(r"[^0-9]", "", raw)
            if value:
                pages[key.strip()] = int(value)
        free_pages = pages.get("Pages free", 0) + pages.get("Pages speculative", 0)
        used_bytes = max(0, total_bytes - (free_pages * page_size))
        out["total_mb"] = total_bytes / 1024 / 1024
        out["used_mb"] = used_bytes / 1024 / 1024
        out["used_pct"] = (used_bytes / total_bytes) * 100 if total_bytes else None
    except (OSError, subprocess.SubprocessError, ValueError):
        pass

    try:
        rss_kb = subprocess.check_output(["/bin/ps", "-o", "rss=", "-p", str(agent_pid)], text=True).strip()
        if rss_kb:
            out["agent_rss_mb"] = int(rss_kb) / 1024
    except (OSError, subprocess.SubprocessError, ValueError):
        pass

    return out


def metrics_payload(session: PtySession) -> dict[str, object]:
    agent = session.metrics()
    tokens = agent.pop("tokens")
    runtime = agent.pop("runtime")
    return {
        "ok": True,
        "ts": time.time(),
        "agent": agent,
        "tokens": tokens,
        "memory": mac_memory(session.pid),
        "runtime": runtime,
    }


def git_head(path: Path) -> str | None:
    try:
        return subprocess.check_output(
            ["git", "-C", str(path), "rev-parse", "HEAD"],
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except (OSError, subprocess.SubprocessError):
        return None


def load_contract() -> dict[str, object] | None:
    try:
        return json.loads(DS4_CONTRACT_PATH.read_text())
    except (OSError, json.JSONDecodeError):
        return None


def contract_payload(state: State) -> dict[str, object]:
    contract = load_contract()
    ds4_head = git_head(state.ds4_root)
    contract_head = contract.get("ds4_revision") if isinstance(contract, dict) else None
    return {
        "ok": True,
        "ds4_root": str(state.ds4_root),
        "ds4_head": ds4_head,
        "contract_status": "match" if ds4_head and ds4_head == contract_head else "mismatch",
        "bridge_http": {
            "endpoints": [
                {"method": "GET", "path": "/health"},
                {"method": "GET", "path": "/metrics"},
                {"method": "GET", "path": "/events"},
                {"method": "GET", "path": "/contract"},
                {"method": "GET", "path": "/profile"},
                {"method": "POST", "path": "/input"},
                {"method": "POST", "path": "/control"},
            ]
        },
        "contract": contract,
    }


def command_option(command: list[str], *names: str) -> str | None:
    for i, value in enumerate(command):
        if value in names and i + 1 < len(command):
            return command[i + 1]
    return None


def command_flag(command: list[str], *names: str) -> bool:
    return any(value in names for value in command)


def infer_backend(command: list[str], runtime: dict[str, object]) -> str:
    if isinstance(runtime.get("backend"), str):
        return str(runtime["backend"])
    backend = command_option(command, "--backend")
    if backend:
        return backend
    if command_flag(command, "--metal"):
        return "metal"
    if command_flag(command, "--cuda"):
        return "cuda"
    if command_flag(command, "--cpu"):
        return "cpu"
    return "metal" if sys.platform == "darwin" else "cuda"


def int_option(command: list[str], name: str) -> int | None:
    value = command_option(command, name)
    if value is None:
        return None
    try:
        return int(value)
    except ValueError:
        return None


def runtime_profile_payload(state: State) -> dict[str, object]:
    contract = load_contract()
    ds4_head = git_head(state.ds4_root)
    contract_head = contract.get("ds4_revision") if isinstance(contract, dict) else None
    metrics = state.session.metrics()
    runtime = metrics.get("runtime", {})
    if not isinstance(runtime, dict):
        runtime = {}
    command = list(state.session.command)
    model_path = command_option(command, "--model", "-m")
    chdir = command_option(command, "--chdir") or state.session.cwd
    ctx = runtime.get("ctx") if isinstance(runtime.get("ctx"), int) else int_option(command, "--ctx")
    profile = {
        "schema_version": 1,
        "id": state.profile_id,
        "kind": state.profile_kind,
        "ownership": state.ownership,
        "platform": sys.platform,
        "ds4_root": str(state.ds4_root),
        "ds4_head": ds4_head,
        "contract_revision": contract_head,
        "contract_status": "match" if ds4_head and ds4_head == contract_head else "mismatch",
        "command": command,
        "command_display": " ".join(shlex.quote(part) for part in command),
        "command_cwd": state.session.cwd,
        "chdir": chdir,
        "model_path": model_path,
        "backend": infer_backend(command, runtime),
        "ctx": ctx or 100000,
        "prefill_chunk": runtime.get("prefill_chunk"),
        "mtp_path": command_option(command, "--mtp"),
        "bridge": {"host": state.host, "port": state.port},
        "process": {
            "pid": metrics.get("pid"),
            "started_at": state.session.started_at,
            "uptime_sec": metrics.get("uptime_sec"),
            "waiting": metrics.get("waiting"),
            "exited": metrics.get("exited"),
            "exit_status": metrics.get("exit_status"),
        },
        "env": {key: state.session.env[key] for key in sorted(state.session.env)},
    }
    return {"ok": True, "profile": profile}


def runtime_state_payload(state: State, ready: bool) -> dict[str, object]:
    return {
        "schema_version": 1,
        "profile_id": state.profile_id,
        "profile": runtime_profile_payload(state)["profile"],
        "bridge_pid": os.getpid(),
        "agent_pid": state.session.pid,
        "ready": ready,
        "token_sha256": hashlib.sha256(state.token.encode("utf-8")).hexdigest(),
        "bridge": {"host": state.host, "port": state.port},
        "updated_at": time.time(),
    }


def write_runtime_state(state: State, ready: bool) -> None:
    atomic_write_json(state.state_file, runtime_state_payload(state, ready))


def control_agent(state: State, payload: dict[str, object]) -> tuple[HTTPStatus, dict[str, object]]:
    action = str(payload.get("action", ""))
    if action == "save":
        state.session.write("/save\r")
        return HTTPStatus.OK, {"ok": True, "action": action}
    if action == "list":
        state.session.write("/list\r")
        return HTTPStatus.OK, {"ok": True, "action": action}
    if action == "history":
        turns = payload.get("turns", 8)
        try:
            turns_int = max(1, min(100, int(turns)))
        except (TypeError, ValueError):
            turns_int = 8
        state.session.write(f"/history {turns_int}\r")
        return HTTPStatus.OK, {"ok": True, "action": action, "turns": turns_int}
    if action == "interrupt":
        state.session.write("\x03")
        return HTTPStatus.OK, {"ok": True, "action": action}
    if action == "restart":
        if state.ownership != "owned":
            return HTTPStatus.CONFLICT, {"ok": False, "error": "cannot restart an attached DS4 runtime"}
        state.session.restart()
        ready = state.session.wait_until_ready(state.ready_timeout)
        write_runtime_state(state, ready)
        if not ready:
            return HTTPStatus.ACCEPTED, {
                "ok": False,
                "action": action,
                "pid": state.session.pid,
                "error": "ds4-agent restarted but did not reach ready state before timeout",
            }
        return HTTPStatus.OK, {"ok": True, "action": action, "pid": state.session.pid, "ready": True}
    return HTTPStatus.BAD_REQUEST, {"ok": False, "error": f"unknown control action: {action}"}


def make_handler(state: State):
    class Handler(BaseHTTPRequestHandler):
        server_version = "DS4WebPTY/0.1"

        def log_message(self, fmt: str, *args) -> None:
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

        def token_from_request(self) -> str:
            parsed = urllib.parse.urlparse(self.path)
            query = urllib.parse.parse_qs(parsed.query)
            if query.get("token"):
                return query["token"][0]
            header = self.headers.get("X-DS4-Token")
            if header:
                return header
            return parse_cookies(self.headers.get("Cookie")).get("ds4_webpty_token", "")

        def authorized(self) -> bool:
            supplied = self.token_from_request()
            return bool(supplied) and secrets.compare_digest(supplied, state.token)

        def send_text(self, status: HTTPStatus, text: str, content_type: str = "text/plain") -> None:
            body = text.encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", content_type + "; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def send_file(self, path: str, content_type: str) -> None:
            try:
                with open(path, "rb") as f:
                    body = f.read()
            except OSError:
                self.send_error(HTTPStatus.NOT_FOUND)
                return
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "public, max-age=3600")
            self.end_headers()
            self.wfile.write(body)

        def send_ui_index(self) -> None:
            index_path = UI_DIST / "index.html"
            if UI_DIST.is_dir() and index_path.is_file():
                try:
                    body = index_path.read_bytes()
                except OSError:
                    self.send_error(HTTPStatus.NOT_FOUND)
                    return
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.send_header(
                    "Set-Cookie",
                    "ds4_webpty_token=%s; SameSite=Lax; Path=/" % urllib.parse.quote(state.token),
                )
                self.end_headers()
                self.wfile.write(body)
                return

            self.send_response(HTTPStatus.SERVICE_UNAVAILABLE)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header(
                "Set-Cookie",
                "ds4_webpty_token=%s; SameSite=Lax; Path=/" % urllib.parse.quote(state.token),
            )
            body = MISSING_UI_HTML.encode("utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def maybe_send_ui_asset(self, parsed_path: str) -> bool:
            if not UI_DIST.is_dir():
                return False
            if parsed_path == "/index.html":
                self.send_ui_index()
                return True
            if not parsed_path.startswith("/assets/"):
                return False
            candidate = (UI_DIST / parsed_path.lstrip("/")).resolve()
            if UI_DIST_ROOT not in (candidate, *candidate.parents):
                self.send_error(HTTPStatus.FORBIDDEN)
                return True
            if not candidate.is_file():
                self.send_error(HTTPStatus.NOT_FOUND)
                return True
            content_type = mimetypes.guess_type(str(candidate))[0] or "application/octet-stream"
            try:
                body = candidate.read_bytes()
            except OSError:
                self.send_error(HTTPStatus.NOT_FOUND)
                return True
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "public, max-age=31536000, immutable")
            self.end_headers()
            self.wfile.write(body)
            return True

        def do_GET(self) -> None:
            parsed = urllib.parse.urlparse(self.path)
            if parsed.path.startswith("/t/"):
                path_token = urllib.parse.unquote(parsed.path[3:])
                if not secrets.compare_digest(path_token, state.token):
                    self.send_text(HTTPStatus.UNAUTHORIZED, "unauthorized")
                    return
                self.send_ui_index()
                return
            if parsed.path in ASSET_PATHS:
                asset_path, content_type = ASSET_PATHS[parsed.path]
                self.send_file(asset_path, content_type)
                return
            if self.maybe_send_ui_asset(parsed.path):
                return
            if parsed.path == "/health":
                metrics = state.session.metrics()
                body = {
                    "ok": True,
                    "pid": state.session.pid,
                    "seq": state.session.seq,
                    "exited": state.session.exit_status is not None,
                    "ready": bool(metrics.get("waiting")) and metrics.get("exit_status") is None,
                    "profile_id": state.profile_id,
                    "ownership": state.ownership,
                }
                self.send_text(HTTPStatus.OK, json.dumps(body), "application/json")
                return
            if parsed.path == "/metrics":
                if not self.authorized():
                    self.send_text(HTTPStatus.UNAUTHORIZED, "unauthorized")
                    return
                self.send_text(HTTPStatus.OK, json.dumps(metrics_payload(state.session)), "application/json")
                return
            if parsed.path == "/contract":
                if not self.authorized():
                    self.send_text(HTTPStatus.UNAUTHORIZED, "unauthorized")
                    return
                self.send_text(HTTPStatus.OK, json.dumps(contract_payload(state)), "application/json")
                return
            if parsed.path == "/profile":
                if not self.authorized():
                    self.send_text(HTTPStatus.UNAUTHORIZED, "unauthorized")
                    return
                self.send_text(HTTPStatus.OK, json.dumps(runtime_profile_payload(state)), "application/json")
                return
            if parsed.path == "/events":
                self.handle_events(parsed)
                return
            if parsed.path != "/":
                self.send_error(HTTPStatus.NOT_FOUND)
                return
            if not self.authorized():
                self.send_text(HTTPStatus.OK, LOGIN_HTML, "text/html")
                return
            self.send_ui_index()

        def do_POST(self) -> None:
            parsed = urllib.parse.urlparse(self.path)
            if parsed.path == "/login":
                length = int(self.headers.get("Content-Length", "0"))
                raw = self.rfile.read(length).decode("utf-8", errors="replace")
                values = urllib.parse.parse_qs(raw)
                token = values.get("token", [""])[0]
                if not secrets.compare_digest(token, state.token):
                    self.send_text(HTTPStatus.UNAUTHORIZED, "bad token")
                    return
                self.send_response(HTTPStatus.SEE_OTHER)
                self.send_header("Location", "/?token=" + urllib.parse.quote(token))
                self.send_header("Set-Cookie", "ds4_webpty_token=%s; SameSite=Lax; Path=/" % urllib.parse.quote(token))
                self.end_headers()
                return
            if parsed.path == "/control":
                if not self.authorized():
                    self.send_text(HTTPStatus.UNAUTHORIZED, "unauthorized")
                    return
                length = int(self.headers.get("Content-Length", "0"))
                raw = self.rfile.read(length)
                try:
                    payload = json.loads(raw.decode("utf-8"))
                except json.JSONDecodeError:
                    self.send_text(HTTPStatus.BAD_REQUEST, json.dumps({"ok": False, "error": "invalid json"}), "application/json")
                    return
                try:
                    status, body = control_agent(state, payload)
                except RuntimeError as exc:
                    status, body = HTTPStatus.CONFLICT, {"ok": False, "error": str(exc)}
                except OSError:
                    status, body = HTTPStatus.CONFLICT, {"ok": False, "error": "agent PTY is no longer accepting input"}
                self.send_text(status, json.dumps(body), "application/json")
                return
            if parsed.path != "/input":
                self.send_error(HTTPStatus.NOT_FOUND)
                return
            if not self.authorized():
                self.send_text(HTTPStatus.UNAUTHORIZED, "unauthorized")
                return
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length)
            try:
                payload = json.loads(raw.decode("utf-8"))
                text = str(payload.get("text", ""))
            except json.JSONDecodeError:
                text = raw.decode("utf-8", errors="replace")
            try:
                state.session.write(text)
            except RuntimeError as exc:
                self.send_text(HTTPStatus.CONFLICT, str(exc))
                return
            except OSError:
                self.send_text(HTTPStatus.CONFLICT, "agent PTY is no longer accepting input")
                return
            self.send_text(HTTPStatus.OK, "ok")

        def handle_events(self, parsed: urllib.parse.ParseResult) -> None:
            if not self.authorized():
                self.send_text(HTTPStatus.UNAUTHORIZED, "unauthorized")
                return
            query = urllib.parse.parse_qs(parsed.query)
            try:
                last = int(query.get("last", [self.headers.get("Last-Event-ID") or "0"])[0] or "0")
            except (TypeError, ValueError):
                last = 0
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            try:
                while True:
                    with state.session.cond:
                        if state.session.chunks:
                            oldest_seq = state.session.chunks[0][0]
                            newest_seq = state.session.chunks[-1][0]
                            if last > newest_seq:
                                last = max(0, oldest_seq - 1)
                        chunks = [(seq, text) for seq, text in state.session.chunks if seq > last]
                        if not chunks and state.session.exit_status is None:
                            state.session.cond.wait(timeout=15)
                            if state.session.chunks:
                                oldest_seq = state.session.chunks[0][0]
                                newest_seq = state.session.chunks[-1][0]
                                if last > newest_seq:
                                    last = max(0, oldest_seq - 1)
                            chunks = [(seq, text) for seq, text in state.session.chunks if seq > last]
                        exit_status = state.session.exit_status
                    for seq, text in chunks:
                        last = seq
                        self.wfile.write(("id: %d\n" % seq).encode("utf-8"))
                        self.wfile.write(b"event: pty\n")
                        payload = json.dumps({"text": text}, ensure_ascii=False)
                        self.wfile.write(("data: %s\n" % payload).encode("utf-8", errors="replace"))
                        self.wfile.write(b"\n")
                        self.wfile.flush()
                    if exit_status is not None:
                        self.wfile.write(b"event: exit\n")
                        self.wfile.write(("data: %s\n\n" % exit_status).encode("utf-8"))
                        self.wfile.flush()
                        return
                    if not chunks:
                        self.wfile.write(b": heartbeat\n\n")
                        self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                return

    return Handler


class DemoHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 128


def lan_ip() -> str:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("8.8.8.8", 80))
        return sock.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        sock.close()


def main() -> int:
    parser = argparse.ArgumentParser(description="Authenticated web PTY for ds4-agent")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18092)
    parser.add_argument("--token", default=os.environ.get("DS4_WEBPTY_TOKEN") or secrets.token_urlsafe(24))
    parser.add_argument("--ds4-root", type=Path, default=Path(os.environ.get("DS4_ROOT", "/Users/mac/ds4")))
    parser.add_argument("--runtime-dir", type=Path, default=RUNTIME_DIR)
    parser.add_argument("--ready-timeout", type=float, default=600.0)
    parser.add_argument("--allow-contract-drift", action="store_true")
    parser.add_argument("--no-reuse-existing", action="store_true")
    parser.add_argument("--cwd")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    ds4_root = args.ds4_root.resolve()
    cwd = args.cwd or str(ds4_root)

    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        command = [
            str(ds4_root / "ds4-agent"),
            "--non-interactive",
            "--chdir",
            str(ds4_root),
            "--model",
            str(ds4_root / "ds4flash.gguf"),
            "--system",
            "You are running through a small web PTY. The user's default code workspace is /Users/mac/webkitium. Use absolute paths for repository files unless the user says otherwise.",
        ]

    kind = profile_kind()
    ident = profile_id(kind, ds4_root, args.port)
    state_file, lock_file = runtime_paths(args.runtime_dir.expanduser(), ident)
    runtime_lock = RuntimeLock(lock_file)
    if not runtime_lock.acquire():
        existing = load_runtime_state(state_file)
        if existing and not args.no_reuse_existing:
            try:
                bridge_pid = int(existing.get("bridge_pid", 0))
            except (TypeError, ValueError):
                bridge_pid = 0
            if pid_alive(bridge_pid):
                print_existing_runtime(existing, args.token)
                return 0
        raise SystemExit(f"DS4 runtime profile is already locked: {lock_file}")

    try:
        preflight_runtime(
            ds4_root=ds4_root,
            command=command,
            cwd=cwd,
            port=args.port,
            allow_contract_drift=args.allow_contract_drift,
        )
    except PreflightError as exc:
        runtime_lock.release()
        raise SystemExit(f"DS4 runtime preflight failed: {exc}") from exc

    env = {
        "TERM": "dumb",
        "COLUMNS": "110",
        "LINES": "38",
    }
    session = PtySession(command=command, cwd=cwd, env=env)
    state = State(
        token=args.token,
        session=session,
        ds4_root=ds4_root,
        host=args.host,
        port=args.port,
        ident=ident,
        state_file=state_file,
        ready_timeout=args.ready_timeout,
    )
    try:
        server = DemoHTTPServer((args.host, args.port), make_handler(state))
    except OSError as exc:
        runtime_lock.release()
        raise SystemExit(f"DS4 runtime preflight failed: cannot bind port {args.port}: {exc}") from exc
    session.start()
    write_runtime_state(state, ready=False)

    def shutdown(signum, frame) -> None:  # type: ignore[no-untyped-def]
        session.terminate()
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    ready = session.wait_until_ready(args.ready_timeout)
    write_runtime_state(state, ready=ready)
    if not ready:
        session.terminate()
        clear_runtime_state(state_file)
        runtime_lock.release()
        raise SystemExit("DS4 runtime preflight failed: ds4-agent did not reach ready state before timeout")

    visible_host = lan_ip() if args.host in ("0.0.0.0", "::") else args.host
    print("DS4 web PTY listening:")
    print("  local: http://127.0.0.1:%d/?token=%s" % (args.port, urllib.parse.quote(args.token)))
    print("  phone: http://%s:%d/?token=%s" % (visible_host, args.port, urllib.parse.quote(args.token)))
    print("  pid: %d" % session.pid)
    sys.stdout.flush()

    try:
        server.serve_forever()
    finally:
        session.terminate()
        clear_runtime_state(state_file)
        runtime_lock.release()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
