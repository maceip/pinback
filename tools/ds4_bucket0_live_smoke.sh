#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
DS4_ROOT="${DS4_ROOT:-/Users/mac/ds4}"
PYTHON="${PYTHON:-python3}"
TOKEN="${TOKEN:-bucket-zero-live-smoke-token}"
PORT="${PORT:-$("$PYTHON" - <<'PY'
import socket

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.bind(("127.0.0.1", 0))
print(sock.getsockname()[1])
sock.close()
PY
)}"

TMPDIR="$(mktemp -d "${TMPDIR:-/tmp}/ds4-bucket0-live.XXXXXX")"
BRIDGE_PID=""

cleanup() {
    if [ -n "$BRIDGE_PID" ] && kill -0 "$BRIDGE_PID" 2>/dev/null; then
        kill "$BRIDGE_PID" 2>/dev/null || true
        wait "$BRIDGE_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT INT TERM

FAKE_AGENT="$TMPDIR/fake-ds4-agent"
cat > "$FAKE_AGENT" <<'SH'
#!/bin/sh
printf 'ds4-agent: context buffers 128.00 MiB (ctx=100000, backend=metal, prefill_chunk=4096, raw_kv_rows=1, compressed_kv_rows=1)\n'
printf '+DWARFSTAR_WAITING\n'
while IFS= read -r line; do
    printf 'live smoke received: %s\n' "$line"
    printf '+DWARFSTAR_WAITING\n'
done
SH
chmod +x "$FAKE_AGENT"

cd "$ROOT_DIR"
"$PYTHON" tools/ds4_agent_webpty.py \
    --host 127.0.0.1 \
    --port "$PORT" \
    --token "$TOKEN" \
    --ds4-root "$DS4_ROOT" \
    --runtime-dir "$TMPDIR/runtime" \
    --ready-timeout 5 \
    --cwd "$TMPDIR" \
    -- "$FAKE_AGENT" > "$TMPDIR/bridge.log" 2>&1 &
BRIDGE_PID="$!"

BASE="http://127.0.0.1:$PORT"

for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    if curl -fsS "$BASE/health" > "$TMPDIR/health.json" 2>/dev/null; then
        if "$PYTHON" - "$TMPDIR/health.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as fh:
    data = json.load(fh)
sys.exit(0 if data.get("ready") is True and data.get("ownership") == "owned" else 1)
PY
        then
            break
        fi
    fi
    sleep 0.25
done

"$PYTHON" - "$TMPDIR/health.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as fh:
    data = json.load(fh)
if data.get("ready") is not True:
    raise SystemExit(f"bridge did not become ready: {data}")
if data.get("ownership") != "owned":
    raise SystemExit(f"bridge is not owned: {data}")
PY

curl -fsS "$BASE/profile?token=$TOKEN" > "$TMPDIR/profile.json"
curl -fsS "$BASE/contract?token=$TOKEN" > "$TMPDIR/contract.json"
curl -fsS "$BASE/metrics?token=$TOKEN" > "$TMPDIR/metrics-before.json"
curl -fsS \
    -H 'Content-Type: application/json' \
    -d '{"text":"bucket zero live smoke\n"}' \
    "$BASE/input?token=$TOKEN" > "$TMPDIR/input.out"
sleep 0.25
curl -fsS "$BASE/metrics?token=$TOKEN" > "$TMPDIR/metrics-after.json"

"$PYTHON" - "$TMPDIR/profile.json" "$TMPDIR/contract.json" "$TMPDIR/metrics-before.json" "$TMPDIR/metrics-after.json" "$TMPDIR/input.out" <<'PY'
import json
import sys

profile_path, contract_path, before_path, after_path, input_path = sys.argv[1:]
profile_payload = json.load(open(profile_path, "r", encoding="utf-8"))
profile = profile_payload.get("profile") if isinstance(profile_payload, dict) else None
contract = json.load(open(contract_path, "r", encoding="utf-8"))
before = json.load(open(before_path, "r", encoding="utf-8"))
after = json.load(open(after_path, "r", encoding="utf-8"))
input_out = open(input_path, "r", encoding="utf-8").read().strip()

if not isinstance(profile, dict):
    raise SystemExit(f"profile payload is invalid: {profile_payload}")
if profile.get("ownership") != "owned":
    raise SystemExit(f"profile is not owned: {profile_payload}")
if not profile.get("id"):
    raise SystemExit(f"profile id missing: {profile_payload}")
if contract.get("contract_status") != "match":
    raise SystemExit(f"contract is not current: {contract}")
if input_out != "ok":
    raise SystemExit(f"input endpoint failed: {input_out!r}")
if after.get("seq", 0) <= before.get("seq", -1):
    raise SystemExit(f"metrics did not advance after input: before={before} after={after}")
PY

"$PYTHON" tools/ds4_agent_webpty.py \
    --host 127.0.0.1 \
    --port "$PORT" \
    --token "$TOKEN" \
    --ds4-root "$DS4_ROOT" \
    --runtime-dir "$TMPDIR/runtime" \
    --ready-timeout 5 \
    --cwd "$TMPDIR" \
    -- "$FAKE_AGENT" > "$TMPDIR/reuse.log" 2>&1

grep -q "already running from owned runtime profile" "$TMPDIR/reuse.log"

echo "bucket zero live smoke: ok"
echo "  port: $PORT"
echo "  bridge pid: $BRIDGE_PID"
