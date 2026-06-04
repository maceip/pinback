#!/usr/bin/env python3
"""Bucket 0 supervisor tests for ds4_agent_webpty.py.

These tests use a tiny fake agent so the supervisor contract can be checked
without loading a GGUF or exercising DS4 inference.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BRIDGE = ROOT / "runtime" / "ds4_agent_webpty.py"
TOKEN = "bucket-zero-test-token"


class TestFailure(AssertionError):
    pass


def assert_true(value: object, message: str) -> None:
    if not value:
        raise TestFailure(message)


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def http_json(port: int, path: str) -> dict[str, object]:
    with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=2) as response:
        return json.loads(response.read().decode("utf-8"))


def http_event_sample(port: int, path: str, timeout: float = 3.0, data_lines: int = 1) -> str:
    with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=timeout) as response:
        lines: list[str] = []
        seen_data = 0
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and len(lines) < 120:
            try:
                line = response.readline().decode("utf-8", errors="replace")
            except TimeoutError:
                break
            if not line:
                break
            lines.append(line)
            if "data:" in line:
                seen_data += 1
                if seen_data >= data_lines:
                    break
        return "".join(lines)


def wait_for_output(proc: subprocess.Popen[str], needle: str, timeout: float) -> str:
    deadline = time.monotonic() + timeout
    out = ""
    while time.monotonic() < deadline:
        if proc.stdout is not None:
            line = proc.stdout.readline()
            if line:
                out += line
                if needle in out:
                    return out
        if proc.poll() is not None:
            err = proc.stderr.read() if proc.stderr is not None else ""
            raise TestFailure(f"process exited before {needle!r}\nstdout:\n{out}\nstderr:\n{err}")
    raise TestFailure(f"timed out waiting for {needle!r}\nstdout:\n{out}")


def stop_process(proc: subprocess.Popen[str]) -> None:
    if proc.poll() is not None:
        return
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        proc.wait(timeout=4)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait(timeout=4)


def listener_pids(port: int) -> list[int]:
    lsof = shutil.which("lsof")
    if not lsof:
        return []
    try:
        output = subprocess.check_output(
            [lsof, "-nP", f"-tiTCP:{port}", "-sTCP:LISTEN"],
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except subprocess.CalledProcessError:
        return []
    return [int(line) for line in output.splitlines() if line.strip().isdigit()]


def write_fake_agent(tmp: Path) -> Path:
    agent = tmp / "fake_agent.py"
    agent.write_text(
        """
import sys
import time

mode = sys.argv[1] if len(sys.argv) > 1 else "ready"
if mode == "ready":
    sys.stdout.write("ds4-agent: context buffers 1.00 MiB (ctx=100000, backend=metal, prefill_chunk=4096, raw_kv_rows=1, compressed_kv_rows=1)\\n")
    sys.stdout.write("+DWARFSTAR_WAITING\\n")
    sys.stdout.flush()
    for line in sys.stdin:
        if line.strip() == "/history 8":
            sys.stdout.write("history ok\\n+DWARFSTAR_WAITING\\n")
        elif line.strip() == "/save":
            sys.stdout.write("save ok\\n+DWARFSTAR_WAITING\\n")
        else:
            sys.stdout.write("echo: " + line + "+DWARFSTAR_WAITING\\n")
        sys.stdout.flush()
elif mode == "silent":
    time.sleep(30)
else:
    raise SystemExit("unknown fake mode: " + mode)
""".lstrip()
    )
    return agent


def bridge_cmd(
    *,
    ds4_root: Path,
    runtime_dir: Path,
    port: int,
    fake_agent: Path,
    mode: str = "ready",
    ready_timeout: float = 2,
    extra_bridge: list[str] | None = None,
    extra_agent: list[str] | None = None,
) -> list[str]:
    return [
        sys.executable,
        "-u",
        str(BRIDGE),
        "--host",
        "127.0.0.1",
        "--port",
        str(port),
        "--token",
        TOKEN,
        "--ds4-root",
        str(ds4_root),
        "--runtime-dir",
        str(runtime_dir),
        "--ready-timeout",
        str(ready_timeout),
        "--cwd",
        str(ds4_root),
        *(extra_bridge or []),
        "--",
        sys.executable,
        str(fake_agent),
        mode,
        *(extra_agent or []),
    ]


def start_bridge(tmp: Path, ds4_root: Path, port: int, fake_agent: Path) -> subprocess.Popen[str]:
    proc = subprocess.Popen(
        bridge_cmd(ds4_root=ds4_root, runtime_dir=tmp / "runtime", port=port, fake_agent=fake_agent),
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    wait_for_output(proc, "DS4 web PTY listening:", 8)
    return proc


def run_bridge_for_failure(cmd: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True, timeout=8)


def make_mismatched_git_root(tmp: Path) -> Path:
    root = tmp / "fake_ds4"
    root.mkdir()
    subprocess.check_call(["git", "init", "-q"], cwd=root)
    subprocess.check_call(
        ["git", "-c", "user.name=DS4 Test", "-c", "user.email=test@example.invalid", "commit", "--allow-empty", "-qm", "init"],
        cwd=root,
    )
    return root


def test_ready_profile_state(tmp: Path, ds4_root: Path, fake_agent: Path) -> None:
    port = free_port()
    proc = start_bridge(tmp, ds4_root, port, fake_agent)
    try:
        health = http_json(port, "/health")
        assert_true(health["ok"] is True, "health ok")
        assert_true(health["ready"] is True, "health ready")

        profile = http_json(port, f"/profile?token={TOKEN}")["profile"]
        assert_true(profile["ownership"] == "owned", "profile ownership")
        assert_true(profile["contract_status"] == "match", "contract status")
        assert_true(profile["bridge"]["port"] == port, "profile port")

        states = list((tmp / "runtime").glob("*.json"))
        assert_true(len(states) == 1, "one runtime state file")
        state = json.loads(states[0].read_text())
        assert_true(state["ready"] is True, "state ready")
        assert_true("token_sha256" in state and "token_hint" not in state, "token is hashed only")
    finally:
        stop_process(proc)
    assert_true(not listener_pids(port), "ready test port cleaned up")


def test_same_profile_reuse(tmp: Path, ds4_root: Path, fake_agent: Path) -> None:
    port = free_port()
    proc = start_bridge(tmp, ds4_root, port, fake_agent)
    try:
        second = run_bridge_for_failure(
            bridge_cmd(ds4_root=ds4_root, runtime_dir=tmp / "runtime", port=port, fake_agent=fake_agent)
        )
        assert_true(second.returncode == 0, "second launch exits successfully")
        assert_true("already running from owned runtime profile" in second.stdout, "second launch reuses runtime")
    finally:
        stop_process(proc)
    assert_true(not listener_pids(port), "reuse test port cleaned up")


def test_events_replay_after_stale_last_seq(tmp: Path, ds4_root: Path, fake_agent: Path) -> None:
    port = free_port()
    proc = start_bridge(tmp, ds4_root, port, fake_agent)
    try:
        sample = http_event_sample(port, f"/events?token={TOKEN}&last=999999", data_lines=3)
        assert_true("event: pty" in sample, "stale event cursor replays pty event")
        assert_true("data:" in sample, "stale event cursor replays data")
        assert_true(sample.count("id: 1\n") == 1, "stale event cursor does not replay forever")
    finally:
        stop_process(proc)
    assert_true(not listener_pids(port), "stale event test port cleaned up")


def test_unknown_port_conflict(tmp: Path, ds4_root: Path, fake_agent: Path) -> None:
    port = free_port()
    blocker = subprocess.Popen(
        [sys.executable, "-m", "http.server", str(port), "--bind", "127.0.0.1"],
        cwd=tmp,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    try:
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline and not listener_pids(port):
            time.sleep(0.05)
        result = run_bridge_for_failure(
            bridge_cmd(ds4_root=ds4_root, runtime_dir=tmp / "runtime-conflict", port=port, fake_agent=fake_agent)
        )
        combined = result.stdout + result.stderr
        assert_true(result.returncode != 0, "port conflict fails")
        assert_true(f"port {port} is already listening" in combined, "port conflict names port")
        assert_true("owner:" in combined, "port conflict names owner")
    finally:
        stop_process(blocker)
    assert_true(not listener_pids(port), "conflict test port cleaned up")


def test_readiness_timeout(tmp: Path, ds4_root: Path, fake_agent: Path) -> None:
    port = free_port()
    result = run_bridge_for_failure(
        bridge_cmd(
            ds4_root=ds4_root,
            runtime_dir=tmp / "runtime-timeout",
            port=port,
            fake_agent=fake_agent,
            mode="silent",
            ready_timeout=0.5,
        )
    )
    combined = result.stdout + result.stderr
    assert_true(result.returncode != 0, "readiness timeout fails")
    assert_true("did not reach ready state" in combined, "readiness timeout is explicit")
    assert_true(not listener_pids(port), "timeout test port cleaned up")


def test_missing_executable(tmp: Path, ds4_root: Path, fake_agent: Path) -> None:
    port = free_port()
    cmd = bridge_cmd(ds4_root=ds4_root, runtime_dir=tmp / "runtime-missing-exe", port=port, fake_agent=fake_agent)
    cmd[-3:] = ["/definitely/not/a/ds4-agent"]
    result = run_bridge_for_failure(cmd)
    combined = result.stdout + result.stderr
    assert_true(result.returncode != 0, "missing executable fails")
    assert_true("DS4 command does not exist" in combined, "missing executable is explicit")


def test_missing_model(tmp: Path, ds4_root: Path, fake_agent: Path) -> None:
    port = free_port()
    result = run_bridge_for_failure(
        bridge_cmd(
            ds4_root=ds4_root,
            runtime_dir=tmp / "runtime-missing-model",
            port=port,
            fake_agent=fake_agent,
            extra_agent=["--model", str(tmp / "missing.gguf")],
        )
    )
    combined = result.stdout + result.stderr
    assert_true(result.returncode != 0, "missing model fails")
    assert_true("model file does not exist" in combined, "missing model is explicit")


def test_contract_drift(tmp: Path, fake_agent: Path) -> None:
    port = free_port()
    fake_ds4 = make_mismatched_git_root(tmp)
    result = run_bridge_for_failure(
        bridge_cmd(ds4_root=fake_ds4, runtime_dir=tmp / "runtime-drift", port=port, fake_agent=fake_agent)
    )
    combined = result.stdout + result.stderr
    assert_true(result.returncode != 0, "contract drift fails")
    assert_true("DS4 contract drift" in combined, "contract drift is explicit")


def run_test(name: str, fn, *args) -> None:  # type: ignore[no-untyped-def]
    sys.stdout.write(f"{name} ... ")
    sys.stdout.flush()
    fn(*args)
    print("ok")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Bucket 0 ds4 webpty supervisor tests")
    parser.add_argument("--ds4-root", type=Path, default=Path("/Users/mac/ds4"))
    args = parser.parse_args()

    ds4_root = args.ds4_root.resolve()
    if not (ds4_root / ".git").is_dir():
        raise SystemExit(f"DS4 git checkout not found: {ds4_root}")

    with tempfile.TemporaryDirectory(prefix="ds4-webpty-supervisor-") as tmp_s:
        tmp = Path(tmp_s)
        fake_agent = write_fake_agent(tmp)
        run_test("ready profile state", test_ready_profile_state, tmp, ds4_root, fake_agent)
        run_test("same profile reuse", test_same_profile_reuse, tmp, ds4_root, fake_agent)
        run_test("events replay after stale cursor", test_events_replay_after_stale_last_seq, tmp, ds4_root, fake_agent)
        run_test("unknown port conflict", test_unknown_port_conflict, tmp, ds4_root, fake_agent)
        run_test("readiness timeout", test_readiness_timeout, tmp, ds4_root, fake_agent)
        run_test("missing executable preflight", test_missing_executable, tmp, ds4_root, fake_agent)
        run_test("missing model preflight", test_missing_model, tmp, ds4_root, fake_agent)
        run_test("contract drift preflight", test_contract_drift, tmp, fake_agent)
    print("bucket zero supervisor tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
