# macOS Shipping Options for DS4 Runtime

## Recommendation

Ship the first macOS-only product as a native macOS app that wraps the existing
web UI in `WKWebView` and starts the existing Python bridge as a local helper.
This is the fastest path that preserves the current `tools/` architecture,
keeps the binary small by using Apple's WebKit instead of Chromium, and lets the
app own the DS4 supervisor lifecycle.

Build the Safari extension only if browser integration is a product requirement.
Safari Web Extensions can talk to native code, but only through a containing
macOS app and its app extension mediator. The extension itself should not be the
runtime owner.

## Current project facts that matter

- The UI is already a static Vite/React bundle in `tools/ds4-agent-ui`.
- The bridge serves that bundle from `tools/ds4-agent-ui/dist`.
- The UI talks to same-origin HTTP/SSE endpoints:
  - `GET /health`
  - `GET /metrics`
  - `GET /events`
  - `GET /contract`
  - `GET /profile`
  - `POST /input`
  - `POST /control`
- `tools/ds4_agent_webpty.py` already owns a DS4 child process, writes runtime
  state, prevents duplicate owned runtimes, and exposes restart/save/history
  controls.
- `tools/ds4_runtime_supervisor.py` is currently a launchd-oriented supervisor
  that also manages an SSH reverse tunnel. That public tunnel behavior should
  not be in the first local desktop app path.
- `tools/com.pinback.ds4-runtime.plist` already shows the intended LaunchAgent
  shape, but it contains hard-coded development paths.

## Option 1: Native macOS WKWebView app plus local helper

### What ships

A signed and notarized `.app` bundle:

```text
DS4 Runtime.app/
  Contents/MacOS/DS4 Runtime          # small Swift/AppKit or SwiftUI launcher
  Contents/Resources/web/             # Vite dist from tools/ds4-agent-ui/dist
  Contents/Resources/bridge/          # Python bridge scripts or frozen helper
  Contents/Resources/ds4-contract.json
```

The app opens a `WKWebView` pointed at:

```text
http://127.0.0.1:<allocated-port>/?token=<random-token>
```

The local helper runs:

```text
ds4_agent_webpty.py --host 127.0.0.1 --port <allocated-port> --token <token> ...
```

### Why this is the fastest/smallest first shipment

- Reuses the current React UI and Python bridge almost unchanged.
- Does not ship Chromium or Electron.
- Gives the app direct ownership of the runtime process, matching the existing
  "owned runtime profile" design.
- Allows a very small first binary if we require an existing Python 3 runtime on
  controlled machines.
- Allows a self-contained external release by swapping the Python scripts for a
  PyInstaller `onedir` helper. That is larger, but still much smaller than an
  Electron app and easier to notarize than `onefile`.

Expected size:

- Swift wrapper plus UI/resources, using system or managed Python: low single
  digit MB plus assets.
- Swift wrapper plus PyInstaller `onedir` Python helper: commonly tens of MB.
- Do not bundle the model file in the app. Treat DS4 root/model path as user or
  installer-provided state.

### Required repository changes

1. Add resource path flags to `tools/ds4_agent_webpty.py`.
   - Add `--ui-dir`.
   - Add `--contract-path`.
   - Add `--asset-root` only if image assets remain outside the Vite bundle.
   - Keep current defaults for development.
   - Use these paths instead of assuming `SCRIPT_DIR / "ds4-agent-ui" / "dist"`.

2. Make launch defaults production-safe.
   - Remove hard-coded `/Users/mac/...` assumptions from the desktop path.
   - Default runtime state to:
     `~/Library/Application Support/DS4 Runtime/runtime`.
   - Persist selected DS4 root and model path in app preferences.

3. Split "local runtime" from "public tunnel supervisor".
   - The desktop helper should start only the local bridge and DS4 child.
   - Keep SSH reverse tunnel supervision as a separate explicit feature.

4. Add a native app target under `native/macos/DS4Runtime`.
   - Use SwiftUI for the window shell or AppKit directly.
   - Use `WKWebView` for the UI.
   - Use `Process` to start/stop the helper.
   - Poll `/health` until `ready: true`, then load the web view.
   - Surface preflight errors in a native error panel.

5. Add a release build script, for example `scripts/build-macos-app.sh`.

### Native app implementation spec

Create these Swift components:

```text
RuntimeProcess.swift
  - allocates a free localhost port
  - creates a cryptographically random token
  - resolves bundled web and bridge resource paths
  - starts the helper with Process
  - streams stdout/stderr to ~/Library/Logs/DS4 Runtime/bridge.log
  - terminates the helper on app quit

BridgeClient.swift
  - GET /health
  - GET /profile?token=...
  - timeout and retry logic while the helper starts

RuntimeSettings.swift
  - selected ds4_root
  - selected model path
  - selected working directory
  - optional extra ds4-agent args

WebShellView.swift
  - owns WKWebView
  - loads http://127.0.0.1:<port>/?token=<token>
  - handles reload/open-devtools debug affordances in development builds
```

Recommended helper command:

```text
python3 tools/ds4_agent_webpty.py \
  --host 127.0.0.1 \
  --port <allocated-port> \
  --token <random-token> \
  --ds4-root <selected-ds4-root> \
  --runtime-dir "$HOME/Library/Application Support/DS4 Runtime/runtime" \
  --ui-dir "$APP/Contents/Resources/web" \
  --contract-path "$APP/Contents/Resources/ds4-interface-contract.json" \
  --cwd <selected-workspace-or-ds4-root> \
  -- \
  <selected-ds4-root>/ds4-agent \
  --non-interactive \
  --chdir <selected-workspace-or-ds4-root> \
  --model <selected-model>
```

Use `127.0.0.1`, not `0.0.0.0`, for the first product. Add LAN/mobile access
later as an explicit user-visible mode.

### Entitlements and signing

For a direct-download app outside the Mac App Store:

- Sign with Developer ID.
- Enable Hardened Runtime.
- Do not enable App Sandbox for the first build unless the file access model has
  been designed around user-selected folders.

If sandboxing is enabled later, the app needs at least:

```xml
<key>com.apple.security.app-sandbox</key><true/>
<key>com.apple.security.network.client</key><true/>
<key>com.apple.security.network.server</key><true/>
<key>com.apple.security.files.user-selected.read-write</key><true/>
```

`network.client` is needed for `WKWebView`. `network.server` is needed because
the helper listens on localhost.

### Build steps

1. Build the web UI:

   ```sh
   cd tools/ds4-agent-ui
   bun install --frozen-lockfile
   bun run build
   ```

2. Run bridge tests:

   ```sh
   cd ../..
   python3 tools/test_ds4_webpty_supervisor.py
   DS4_ROOT=/path/to/ds4 tools/ds4_bucket0_live_smoke.sh
   ```

3. Build the native app:

   ```sh
   xcodebuild \
     -project native/macos/DS4Runtime/DS4Runtime.xcodeproj \
     -scheme "DS4 Runtime" \
     -configuration Release \
     -archivePath build/DS4Runtime.xcarchive \
     archive
   ```

4. Copy runtime resources during the Xcode build phase:

   ```sh
   rsync -a tools/ds4-agent-ui/dist/ \
     "$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/web/"
   rsync -a tools/ds4_agent_webpty.py tools/ds4_contract.py \
     "$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/bridge/"
   cp tools/ds4-agent-ui/ds4-interface-contract.json \
     "$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/ds4-interface-contract.json"
   ```

5. For a self-contained Python helper, build on macOS with PyInstaller `onedir`:

   ```sh
   python3 -m venv .venv-macos
   . .venv-macos/bin/activate
   python -m pip install --upgrade pip pyinstaller
   pyinstaller \
     --onedir \
     --name ds4-bridge \
     --target-architecture universal2 \
     --add-data "tools/ds4-agent-ui/dist:web" \
     --add-data "tools/ds4-agent-ui/ds4-interface-contract.json:." \
     tools/ds4_agent_webpty.py
   ```

   Prefer `onedir` over `onefile` for notarized builds so nested libraries can
   be signed correctly.

6. Export, sign, notarize, and staple:

   ```sh
   xcodebuild -exportArchive \
     -archivePath build/DS4Runtime.xcarchive \
     -exportPath build/export \
     -exportOptionsPlist native/macos/ExportOptions.plist

   codesign --verify --deep --strict --verbose=2 \
     "build/export/DS4 Runtime.app"

   ditto -c -k --sequesterRsrc --keepParent \
     "build/export/DS4 Runtime.app" build/DS4Runtime.zip

   xcrun notarytool submit build/DS4Runtime.zip \
     --keychain-profile "notary-profile" \
     --wait

   xcrun stapler staple "build/export/DS4 Runtime.app"
   ```

### Acceptance checklist

- The app launches on a clean macOS machine.
- The app asks for or discovers DS4 root and model path.
- The helper prints `DS4 web PTY listening:` and `/health` returns
  `ready: true`.
- The web view loads the existing React UI.
- `/profile` shows ownership `owned`, kind `agent-local-metal`, the selected
  DS4 root, model path, backend, context, and process pid.
- Chat input reaches the DS4 PTY through `POST /input`.
- Save/history/restart call `POST /control` successfully.
- Quitting the app terminates the helper unless the user has explicitly enabled
  a background LaunchAgent mode.
- A second app launch reuses or cleanly replaces the owned runtime without a
  port conflict.

## Option 2: Safari Web Extension plus containing macOS app/LaunchAgent

### What ships

A macOS containing app with a Safari Web Extension target:

```text
DS4 Safari.app/
  Contents/MacOS/DS4 Safari                 # containing app
  Contents/PlugIns/DS4 Extension.appex      # Safari Web Extension mediator
  Contents/Resources/bridge/                # same helper as option 1
  Contents/Resources/web-extension/         # popup/runtime page assets
```

The Safari extension provides the browser entry point. The containing macOS app
or a user LaunchAgent owns the supervisor.

### Feasibility answer

Yes, Safari can be used, but not as "just an extension that implants Python".
Safari Web Extensions communicate with native code only through the containing
macOS app's native app extension mediator. The extension itself should be
treated as a UI/control surface. Runtime ownership belongs to:

1. the containing macOS app while it is running, or
2. a user LaunchAgent registered by the containing app.

For this project, the LaunchAgent model is the better extension architecture
because the DS4 runtime is long-lived and process-owning, while Safari extension
handlers are request/response mediators.

### Why choose this

Choose this only if the product needs Safari-native placement:

- toolbar button
- browser page integration
- content-script driven workflows
- Safari-distributed extension experience

Do not choose it only to make the binary smaller. It still requires a containing
app, extension target, native messaging, user enablement in Safari, and a helper
or LaunchAgent for the Python/DS4 runtime.

### Required repository changes

1. Add a bridge base URL mode to `tools/ds4-agent-ui/src/ds4Bridge.ts`.
   - Current UI assumes same-origin paths like `/metrics`.
   - Extension pages need absolute URLs such as
     `http://127.0.0.1:<port>/metrics?token=...`.
   - Add a single resolver:

     ```ts
     const bridgeBase =
       window.localStorage.getItem("ds4_bridge_base_url") || window.location.origin;

     function bridgeUrl(path: string, token: string, extra?: Record<string, string>) {
       const url = new URL(path, bridgeBase);
       const query = authParams(token, extra);
       url.search = query;
       return url.toString();
     }
     ```

   - Ensure `EventSource` also receives an absolute URL.

2. Add an extension bootstrap page.
   - On load, call `browser.runtime.sendNativeMessage(...)`.
   - Request `{ "type": "runtime.ensureStarted" }`.
   - Store returned `baseUrl` and `token`.
   - Render the existing React app after runtime discovery.

3. Add a containing app target under `native/macos/DS4Safari`.

4. Add a Safari Web Extension target under the same Xcode project.

5. Add a LaunchAgent plist template.
   - Use `~/Library/LaunchAgents/com.pinback.ds4-runtime.plist`.
   - Replace development paths with app bundle or Application Support paths.
   - Write logs to `~/Library/Logs/DS4 Runtime/`.

6. Modify `tools/ds4_runtime_supervisor.py`.
   - Add `--no-tunnel` and make it the extension default.
   - Make `public_host` and SSH tunnel optional.
   - Write state containing local `baseUrl`, `token_hash`, `bridge_port`, and
     pid information for the native app/extension to read.

### Extension architecture

```text
Safari toolbar/popup/runtime.html
  -> browser.runtime.sendNativeMessage({type: "runtime.ensureStarted"})
    -> SafariWebExtensionHandler.beginRequest(...)
      -> containing app or XPC service
        -> registers/starts LaunchAgent if needed
        -> reads supervisor state
        -> returns {baseUrl, token, profileId}
  -> React UI fetches bridgeBase + /metrics, /events, /input, /control
```

Important constraints:

- Content scripts cannot be the runtime owner.
- Do not expose the token to arbitrary web pages.
- Prefer extension pages or popup UI for the DS4 cockpit.
- If content scripts need DS4 actions, route them through background script
  messages and enforce an allowlist.

### Manifest shape

Use the manifest version Xcode's Safari Web Extension template generates. The
extension needs at least:

```json
{
  "name": "DS4 Runtime",
  "permissions": ["nativeMessaging", "storage"],
  "host_permissions": [
    "http://127.0.0.1/*",
    "http://localhost/*"
  ],
  "action": {
    "default_popup": "runtime.html"
  },
  "background": {
    "service_worker": "background.js",
    "type": "module"
  }
}
```

Safari routes `runtime.sendNativeMessage` to the containing app extension. The
application id argument used by Chromium native messaging is not the security
boundary on Safari.

### Native message contract

Requests:

```json
{ "type": "runtime.ensureStarted" }
{ "type": "runtime.status" }
{ "type": "runtime.stop" }
{ "type": "runtime.openContainingApp" }
```

Responses:

```json
{
  "ok": true,
  "baseUrl": "http://127.0.0.1:18092",
  "token": "<session-token>",
  "profileId": "agent-local-metal-...",
  "ownership": "owned"
}
```

Errors:

```json
{
  "ok": false,
  "error": "DS4 root is not configured",
  "recover": "openContainingApp"
}
```

### Containing app responsibilities

- First-run onboarding for DS4 root, model path, and workspace path.
- Register/unregister the user LaunchAgent.
- Start/stop/restart the local supervisor.
- Store non-secret preferences in an App Group defaults suite.
- Store tokens in Keychain or generate per-launch tokens and pass them only to
  the extension page.
- Show clear runtime status if Safari is closed.

### Build steps

1. Build the UI in extension mode:

   ```sh
   cd tools/ds4-agent-ui
   bun install --frozen-lockfile
   DS4_EXTENSION=1 bun run build
   ```

2. Copy the built UI into the Safari Web Extension target resources:

   ```sh
   rsync -a tools/ds4-agent-ui/dist/ \
     native/macos/DS4Safari/DS4Extension/Resources/
   ```

3. Build the helper exactly as in option 1, either script-based for internal
   builds or PyInstaller `onedir` for external builds.

4. Build and archive:

   ```sh
   xcodebuild \
     -project native/macos/DS4Safari/DS4Safari.xcodeproj \
     -scheme "DS4 Safari" \
     -configuration Release \
     -archivePath build/DS4Safari.xcarchive \
     archive
   ```

5. Export, sign, notarize, and staple as a macOS app bundle.

6. On first launch, the containing app should:
   - install/register the LaunchAgent,
   - ask the user to enable the Safari extension,
   - verify `/health`,
   - open the extension's runtime page or Safari extension preferences.

### Acceptance checklist

- The containing app installs and launches without command-line setup.
- The user can enable the Safari extension.
- The extension requests runtime status through native messaging.
- The containing app or LaunchAgent starts the local bridge.
- The extension receives `baseUrl` and token.
- The React UI can call `/metrics`, subscribe to `/events`, and post `/input`
  across the localhost origin.
- Closing Safari does not kill the runtime if background supervision is enabled.
- Disabling the LaunchAgent stops background supervision cleanly.
- Content scripts cannot access the token directly.

## Decision matrix

| Criterion | WKWebView app | Safari extension plus app |
| --- | --- | --- |
| Fastest first build | Best | Slower |
| Smallest renderer footprint | Best | Best |
| Reuses current same-origin UI | Best | Needs bridge base URL work |
| Owns supervisor cleanly | Best | Only through containing app/LaunchAgent |
| Browser integration | Limited | Best |
| User install friction | Normal app install | App install plus Safari enablement |
| App Store review risk | Lower for direct download | Higher if distributing through Safari/App Store flow |
| Long-running Python/DS4 runtime | Straightforward | Must not live inside extension handler |

## Prescriptive path

1. Build option 1 first.
2. Keep the helper API identical to today's `/profile`, `/metrics`, `/events`,
   `/input`, and `/control` surface.
3. Make the desktop app own runtime selection, launch, health, and shutdown.
4. Add `--ui-dir` and `--contract-path` before writing native code; those flags
   are the seam that lets both options reuse the same helper.
5. Add Safari extension support only after option 1 is usable, because option 2
   should reuse the same helper, settings, state files, and launch agent logic.
