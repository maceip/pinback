# pinback ingress

`pinback-server` listens on plain HTTP/1.1 by design. TLS, auth, and
public exposure are handled by an ingress in front of it.

Pick the path that matches how secure / public the deployment needs to be.

## Local-only (default)

```
pinback-server
# binds 127.0.0.1:8088, browser at http://127.0.0.1:8088
```

Nothing to configure. Use this on the same machine that runs DS4.

## Tailscale (private network)

Easiest way to reach pinback from a phone or laptop on the same tailnet.

```
sudo bash ingress/tailscale-funnel.sh   # public via Funnel
# OR
tailscale serve https / http://127.0.0.1:8088   # tailnet-only
```

`tailscale-funnel.sh` enables Funnel — the URL is reachable from anywhere
without opening a port.  Disable with `sudo tailscale funnel off`.

## Caddy (public domain, ACME TLS)

```
sudo cp ingress/Caddyfile /etc/caddy/Caddyfile
# edit the host name, basicauth hash
sudo systemctl reload caddy
```

Caddy auto-provisions Let's Encrypt and forwards to `127.0.0.1:8088`.
SSE and WebSocket pass through with `flush_interval -1` and zero idle
timeout. The `header_up X-Forwarded-*` lines preserve the original
client identity.

## WireGuard VM (private mesh)

For a self-hosted VPN with no third-party identity provider:

```
# On the VM:
sudo cp ingress/wg-quick.conf.example /etc/wireguard/pinback.conf
sudo wg-quick up pinback
sudo pinback-server --bind 10.13.13.1:8088 \
  --upstream 127.0.0.1:8000   # if ds4-server runs on the same VM

# On the client (laptop, phone):
# install WireGuard client, paste the matching peer config, connect.
# Browser at http://10.13.13.1:8088.
```

This is the same shape as the original Pinback, just hardened. Pair it
with `tailscale serve` if you also want SSO via Tailscale.

## Notes

- pinback emits HSTS, CSP, `nosniff`, `Permissions-Policy` and
  `Referrer-Policy` headers itself; the proxy may add to them but
  shouldn't replace them.
- All control endpoints (`/api/input`, `/api/control`) are POST and
  same-origin. Never expose pinback to the open internet without
  authentication in front of it; auth is the proxy's job.
- `/healthz` and `/readyz` are intentionally unauthenticated so probes
  work; they leak no state beyond reachability.
