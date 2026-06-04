#!/usr/bin/env bash
# Expose the local pinback-server through Tailscale Funnel.
#
# Pre-reqs:
#   - tailscale CLI installed and logged in
#   - Funnel enabled for this device in the admin console
#     (https://login.tailscale.com/admin/settings/funnel)
#   - pinback-server bound to 127.0.0.1:8088 (the default)
#
# Run as root or with sudo. Idempotent.

set -euo pipefail

PORT="${PORT:-8088}"
URL_PORT="${FUNNEL_PORT:-443}"

if ! command -v tailscale >/dev/null; then
    echo "tailscale not found in PATH" >&2
    exit 1
fi

# Reset any previous serve config to avoid stacking proxies.
tailscale serve reset || true

# Forward https://<tailnet-name>.ts.net/  ->  http://127.0.0.1:$PORT
tailscale serve --bg --https="$URL_PORT" "http://127.0.0.1:$PORT"
tailscale funnel --bg --https="$URL_PORT" on

echo "Funnel up. URLs:"
tailscale serve status

cat <<'TIP'

Tip: run pinback-server with the default --bind 127.0.0.1:8088. Tailscale
handles TLS and identity (SSO into your tailnet). To turn this off:
    sudo tailscale funnel off
    sudo tailscale serve reset
TIP
