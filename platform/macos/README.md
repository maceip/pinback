# macOS shell — AppKit + WKWebView (Objective-C)

A native macOS window hosting `WKWebView` in pure Objective-C (AppKit, no Swift
runtime). The window chrome and WebKit engine are the OS's, so this paints like a
SwiftUI shell while producing a much smaller binary.

## Layout

```
macos/
├── Package.swift                         # SwiftPM executable (ObjC target)
├── Scripts/bundle.sh                     # wrap the binary into a Pinback.app
└── Sources/PinbackShell/main.m             # the entire app
```

## Requirements

- macOS 13+ and a recent Xcode (ObjC + WKWebView; no macOS 26 SDK required).

## How it loads pinback

The shell self-hosts the cockpit: at launch it spawns a bundled `pinback-server`
on `127.0.0.1:8088`, waits for `GET /healthz`, then loads it — and terminates the
server on quit. So a user just opens the app; there is no server to start
separately. Set `PINBACK_URL` to skip spawning and load a server you run yourself
(dev/remote). `PINBACK_SERVER_BIN` overrides the server binary location.

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
make pinback-server
PINBACK_URL=http://127.0.0.1:8088 swift run
```

For an icon/entitlements/signing, open `Package.swift` in Xcode and archive.

## Notes

- No engine is bundled: `WKWebView` is the system WebKit. `pinback-server` is a
  separate binary embedded in the app bundle, not part of the shell binary.
