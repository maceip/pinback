# macOS shell — SwiftUI `WebView` (WKWebView)

A native macOS window hosting `WKWebView` through the **new declarative SwiftUI
`WebView` + `WebPage` API** (WebKit-for-SwiftUI, macOS 26 "Tahoe", currently beta
as of Xcode 26). This replaces the old `NSViewRepresentable` boilerplate — the
whole shell is ~25 lines.

## Layout

```
macos/
├── Package.swift                         # SwiftPM executable, macOS 26 platform
├── Scripts/bundle.sh                     # wrap the binary into a Pinback.app
└── Sources/PinbackShell/PinbackShellApp.swift   # the entire app
```

Scaffolded per the brief with `swift package init --type executable` and then
trimmed to the SwiftUI App entry point.

## Requirements

- macOS 26 SDK + Xcode 26 (the `WebView`/`WebPage` symbols live in `import WebKit`).
- Swift 6.1 toolchain.

## How it loads pinback

The shell self-hosts the cockpit: at launch it spawns a bundled `pinback-server`
on `127.0.0.1:8088`, waits for `GET /healthz`, then loads it — and terminates the
server on quit. So a user just opens the app; there is no server to start
separately. Set `PINBACK_URL` to skip spawning and load a server you run yourself
(dev/remote). `PINBACK_MODEL` / `PINBACK_AGENT_BIN` are passed through to the
spawned server (`--model` / `--agent-bin`). The launch/supervise logic lives in
`Sources/PinbackShell/PinbackServer.swift`.

## Run

For real use, build the bundle — it embeds `pinback-server` and gets an
`Info.plist` (ATS `NSAllowsLocalNetworking`, required for the loopback http load;
a bare `swift run` binary has none and ATS blocks it):

```sh
cd platform/macos
./Scripts/bundle.sh        # builds the shell + pinback-server, assembles Pinback.app
open Pinback.app
```

Quick UI iteration against a server you start yourself:

```sh
PINBACK_URL=http://127.0.0.1:8088 swift run
```

For an icon/entitlements/signing, open `Package.swift` in Xcode 26 and archive.

## Notes

- No engine is bundled: `WKWebView` is the system WebKit. `pinback-server` is a
  separate ~3 MB binary embedded in the app bundle, not part of the shell binary.
- `WebPage` is an `@Observable` model — bind `page.url`, `page.title`,
  `page.estimatedProgress`, etc. for navigation chrome if you ever want more than
  a bare shell.
