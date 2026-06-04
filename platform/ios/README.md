# iOS shell — SwiftUI `WebView` (WKWebView)

A native iOS app hosting `WKWebView` through the **new declarative SwiftUI
`WebView` + `WebPage` API** (WebKit-for-SwiftUI, iOS 26, beta as of Xcode 26).
The app code is identical in spirit to the macOS shell — ~25 lines of SwiftUI.

iOS apps cannot be produced by a bare SwiftPM executable (no `.app`/code-signing),
so this is a real `.xcodeproj`. It is kept minimal by using **Xcode 16+
synchronized file groups** (`PBXFileSystemSynchronizedRootGroup`): the project
references the `Pinback/` folder, not individual files, so adding a `.swift` file
needs no project edit.

## Layout

```
ios/
├── Pinback.xcodeproj/            # tiny pbxproj (synchronized group, objectVersion 77)
└── Pinback/
    ├── PinbackApp.swift          # the entire app
    └── Info.plist                # ATS: NSAllowsLocalNetworking for the http dev server
```

## Requirements

- Xcode 26 (the `WebView`/`WebPage` symbols are in `import WebKit`, iOS 26 SDK).
- Deployment target: iOS 26.0.

## Build & run

Open in Xcode and run on a simulator/device:

```sh
open platform/ios/Pinback.xcodeproj
```

…or from the command line (the `Pinback` scheme is shared):

```sh
cd platform/ios
./build.sh
# or explicitly:
xcodebuild -scheme Pinback -destination 'platform=iOS Simulator,name=iPhone 17' build
```

List available simulators with `xcrun simctl list devices available`. Xcode 26
ships iPhone 17 simulators by default (not iPhone 16).

**Xcode note:** `Info.plist` is excluded from the synchronized `Pinback/` folder
via `PBXFileSystemSynchronizedBuildFileExceptionSet` so Xcode does not copy it
twice (which causes a duplicate-Info.plist build error when
`GENERATE_INFOPLIST_FILE = NO`).

iOS can't host `pinback-server` (no `ds4-agent` / 87 GB model on-device), so the
app is a thin client onto a remote pinback. The iOS Simulator shares the Mac's
network stack, so the default `http://127.0.0.1:8088` reaches a `pinback-server`
running on your Mac (`make make pinback-server && ./pinback-servermake pinback-server && ./pinback-server ./build/pinback-server`). On a physical
device, set `PINBACK_URL` in the scheme's Run → Arguments → Environment to your
Mac's LAN / Tailscale address (and that host's ATS rules apply).

## Notes

- No engine is bundled: `WKWebView` is the system WebKit.
- Set the bundle id (`dev.pinback.shell`) and your signing team in Signing &
  Capabilities before running on a device.
