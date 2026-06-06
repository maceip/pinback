# Pinback shell webview contract

Every platform shell is a **thin webview host**. Product behavior lives in shared
web assets; shells only implement this contract.

## Pages

| Page | Source | When shown |
|------|--------|------------|
| **Setup** | `platform/common/setup.html` + `pinback-host.js` | No server at boot, or user opens server settings |
| **Cockpit** | `ui/app/` (embedded in `pinback-server`) | `GET /healthz` succeeds |

Do not fork setup UI per platform (no duplicate HTML in `android/assets/`, no
one-off native connect screens). iOS loads the same bundled `HostAssets/` as
Android.

## Boot sequence (all shells)

1. Resolve URL: `PINBACK_URL` → persisted config → platform default
2. `GET {url}/healthz` (use `pinback_health_ok()` on desktop)
3. On success → load cockpit URL
4. On failure → desktop may spawn bundled `pinback-server` and retry once
5. Still failing → load `setup.html` from disk (`pinback_setup_file_uri()`)

## JavaScript bridges

Shared implementation: **`platform/common/pinback-host.js`** (copied into
`ui/app/` at embed time; bundled next to `setup.html` on each shell).

### Setup page → native (`pinback-setup`)

```json
{ "type": "pinback-setup", "action": "test"|"connect", "url": "http://host:8088" }
```

Native must run health check and either load cockpit (`connect` + ok) or update
`#status` in the setup page via `evaluateJavaScript`.

Optional native hooks for setup (Android `@JavascriptInterface` on `PinbackSetup`):

- `getSavedUrl()` — last persisted URL
- `getDefaultUrl()` — platform dev default (e.g. emulator `10.0.2.2:8088`)

Desktop uses `pinback_url_save()` / `pinback_url_load_saved()` in C.

### Cockpit → native (`workspaces`)

```json
{ "type": "workspaces", "activeId": "…", "canGoBack": true, "workspaces": […] }
```

Posted on every workspace list change (`window.pinbackPublish()` in `ui/app`).

Channels: `webkit.messageHandlers.pinback`, `PinbackHost.post` (Android),
`chrome.webview.postMessage` (Windows).

### Cockpit → native (`pinback-host`)

```json
{ "type": "pinback-host", "action": "openSetup" }
```

Opens the setup page (menu item, or cockpit offline overlay **Server settings**).
Every shell must handle this on the **cockpit** bridge — not only Android.

### Native → cockpit

- `window.pinback.selectWorkspace(id)`
- `window.pinback.back()` / `refresh()` / `state()`

## Cockpit offline recovery

When the cockpit is loaded but the server stops, **`ui/app`** shows a branded
overlay (`#server-offline`). It probes `/healthz` and calls
`pinbackHost.openServerSetup()` when a native shell is present.

Shells do not implement their own mid-session error pages.

## Platform-specific glue (allowed)

- WebView load-error callbacks → call `load_setup_prefill()`
- Android cleartext HTTP policy (`network_security_config.xml`)
- Emulator vs device default URL (`getDefaultUrl()`)
- Desktop: spawn/kill `pinback-server` child

Everything else belongs in `platform/common/` or `ui/app/`.
