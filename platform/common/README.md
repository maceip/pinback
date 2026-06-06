# platform/common/

Shared helpers used by desktop shells (not duplicated in mobile Kotlin/Swift).

| File | Purpose |
|------|---------|
| `pinback_url.c` / `pinback_url.h` | Resolve server URL (`PINBACK_URL` → saved config → default), `GET /healthz`, persist URL |
| `setup.html` | Pre-connection panel (bundled by every shell) |
| `pinback-host.js` | Shared JS bridge for setup + cockpit (`pinback-setup`, `pinback-host`, workspaces) |

See [`../CONTRACT.md`](../CONTRACT.md) for the full webview host protocol.

Saved URL location:

- macOS / Linux: `$XDG_CONFIG_HOME/pinback/server.url` (typically `~/.config/pinback/server.url`)
- Windows: `%APPDATA%\Pinback\server.url`

Mobile apps use their own storage (`@AppStorage` / `SharedPreferences`) with the same resolution order.
