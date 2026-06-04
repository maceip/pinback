#!/usr/bin/env python3
"""Supervisor for the DS4 web PTY demo bridge and AWS reverse tunnels."""

from __future__ import annotations

import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence


TOKEN = "GSfRP4wGs4aMmRwqafyZenVDEKRiyzNn"


def now() -> float:
    return time.time()


def stamp() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S%z")


def log(message: str) -> None:
    print(f"{stamp()} {message}", flush=True)


def tcp_listening(host: str, port: int, timeout: float = 1.5) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def http_status(url: str, timeout: float = 5.0) -> int:
    request = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return int(response.status)
    except urllib.error.HTTPError as exc:
        return int(exc.code)
    except OSError:
        return 0


def json_get(url: str, timeout: float = 5.0) -> dict[str, object] | None:
    request = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.loads(response.read().decode("utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


@dataclass
class ManagedProcess:
    name: str
    argv: list[str]
    log_path: Path
    restart_delay: float = 5.0
    process: subprocess.Popen[bytes] | None = None
    last_start: float = 0.0
    restarts: int = 0
    _log_file: object | None = field(default=None, init=False, repr=False)

    def running(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def start(self) -> None:
        if self.running():
            return
        if now() - self.last_start < self.restart_delay:
            return
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self._log_file = self.log_path.open("ab", buffering=0)
        self.process = subprocess.Popen(
            self.argv,
            stdin=subprocess.DEVNULL,
            stdout=self._log_file,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        self.last_start = now()
        self.restarts += 1
        log(f"{self.name}: started pid={self.process.pid} restart={self.restarts}")

    def poll(self) -> int | None:
        if self.process is None:
            return None
        status = self.process.poll()
        if status is not None:
            log(f"{self.name}: exited status={status}")
            self._close_log()
            self.process = None
        return status

    def stop(self) -> None:
        if self.process is None:
            self._close_log()
            return
        proc = self.process
        if proc.poll() is None:
            log(f"{self.name}: stopping pid={proc.pid}")
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            deadline = now() + 10
            while now() < deadline and proc.poll() is None:
                time.sleep(0.1)
            if proc.poll() is None:
                log(f"{self.name}: killing pid={proc.pid}")
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        self.process = None
        self._close_log()

    def restart(self) -> None:
        self.stop()
        self.last_start = 0
        self.start()

    def _close_log(self) -> None:
        if self._log_file is not None:
            try:
                self._log_file.close()  # type: ignore[attr-defined]
            finally:
                self._log_file = None


def bridge_command(args: argparse.Namespace) -> list[str]:
    ds4_root = Path(args.ds4_root).resolve()
    return [
        sys.executable,
        str(Path(__file__).resolve().parent / "ds4_agent_webpty.py"),
        "--host",
        "127.0.0.1",
        "--port",
        str(args.bridge_port),
        "--token",
        args.token,
        "--ds4-root",
        str(ds4_root),
        "--runtime-dir",
        str(Path(args.runtime_dir).expanduser().resolve()),
        "--ready-timeout",
        str(args.ready_timeout),
    ]


def tunnel_command(args: argparse.Namespace) -> list[str]:
    return [
        "ssh",
        "-N",
        "-o",
        "BatchMode=yes",
        "-o",
        "ExitOnForwardFailure=yes",
        "-o",
        "ServerAliveInterval=15",
        "-o",
        "ServerAliveCountMax=2",
        "-o",
        "StrictHostKeyChecking=accept-new",
        "-R",
        f"127.0.0.1:{args.bridge_port}:127.0.0.1:{args.bridge_port}",
        "-R",
        f"127.0.0.1:{args.preview_port}:127.0.0.1:{args.preview_port}",
        args.ssh_target,
    ]


def write_state(path: Path, args: argparse.Namespace, bridge: ManagedProcess, tunnel: ManagedProcess) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "updated_at": stamp(),
        "public_chat_url": f"http://{args.public_host}/t/{args.token}",
        "public_preview_url": f"http://{args.public_host}/preview/",
        "bridge_port": args.bridge_port,
        "preview_port": args.preview_port,
        "bridge_pid": bridge.process.pid if bridge.running() and bridge.process else None,
        "tunnel_pid": tunnel.process.pid if tunnel.running() and tunnel.process else None,
        "bridge_restarts": bridge.restarts,
        "tunnel_restarts": tunnel.restarts,
    }
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    os.replace(tmp, path)


def healthy_bridge(args: argparse.Namespace) -> bool:
    payload = json_get(f"http://127.0.0.1:{args.bridge_port}/health")
    if not payload:
        return False
    return bool(payload.get("ok")) and payload.get("exited") is False


def healthy_public(args: argparse.Namespace) -> tuple[bool, bool]:
    chat = http_status(f"http://{args.public_host}/health")
    preview = http_status(f"http://{args.public_host}/preview/")
    return chat == 200, preview == 200


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Supervise DS4 web PTY and AWS reverse tunnels")
    parser.add_argument("--token", default=os.environ.get("DS4_WEBPTY_TOKEN", TOKEN))
    parser.add_argument("--public-host", default="35.158.168.87")
    parser.add_argument("--ssh-target", default="ubuntu@35.158.168.87")
    parser.add_argument("--bridge-port", type=int, default=18092)
    parser.add_argument("--preview-port", type=int, default=18093)
    parser.add_argument("--ds4-root", default="/Users/mac/ds4")
    parser.add_argument("--runtime-dir", default="/Users/mac/pinback/.runtime/ds4")
    parser.add_argument("--state-file", default="/Users/mac/pinback/.runtime/ds4-supervisor/state.json")
    parser.add_argument("--log-dir", default="/Users/mac/pinback/.runtime/ds4-supervisor/logs")
    parser.add_argument("--ready-timeout", type=float, default=600.0)
    parser.add_argument("--check-interval", type=float, default=10.0)
    parser.add_argument("--bridge-failures-before-restart", type=int, default=3)
    parser.add_argument("--public-failures-before-restart", type=int, default=3)
    return parser.parse_args(argv)


def main(argv: Sequence[str] = sys.argv[1:]) -> int:
    args = parse_args(argv)
    log_dir = Path(args.log_dir).expanduser().resolve()
    state_file = Path(args.state_file).expanduser().resolve()
    bridge = ManagedProcess("bridge", bridge_command(args), log_dir / "bridge.log", restart_delay=15)
    tunnel = ManagedProcess("tunnel", tunnel_command(args), log_dir / "tunnel.log", restart_delay=5)
    stopping = False
    bridge_failures = 0
    public_failures = 0

    def stop(_signum: int, _frame: object) -> None:
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    log("supervisor: starting")
    log(f"supervisor: chat=http://{args.public_host}/t/{args.token}")
    log(f"supervisor: preview=http://{args.public_host}/preview/")

    try:
        while not stopping:
            bridge.poll()
            tunnel.poll()
            if not bridge.running():
                bridge.start()
            if not tunnel.running():
                tunnel.start()

            if bridge.running() and not healthy_bridge(args):
                bridge_failures += 1
                log("bridge: local health failed")
                if bridge_failures >= args.bridge_failures_before_restart:
                    log("bridge: restarting after repeated local health failures")
                    bridge.restart()
                    bridge_failures = 0
            else:
                bridge_failures = 0

            chat_ok, preview_ok = healthy_public(args)
            if chat_ok and preview_ok:
                public_failures = 0
            else:
                public_failures += 1
                log(
                    "public: unhealthy "
                    f"chat={chat_ok} preview={preview_ok} failures={public_failures}"
                )
                if public_failures >= args.public_failures_before_restart:
                    log("public: restarting tunnel after repeated failures")
                    tunnel.restart()
                    public_failures = 0

            write_state(state_file, args, bridge, tunnel)
            time.sleep(args.check_interval)
    finally:
        log("supervisor: stopping")
        tunnel.stop()
        bridge.stop()
        write_state(state_file, args, bridge, tunnel)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
