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
xcodebuild -scheme Pinback -destination 'platform=iOS Simulator,name=iPhone 16' build
```

The iOS Simulator shares the Mac's network stack, so the default
`http://127.0.0.1:18192` reaches the cockpit dev server running on your Mac. On a
physical device, set `PINBACK_URL` in the scheme's Run → Arguments → Environment
to your Mac's LAN address (and that host's ATS rules apply).

## Notes

- No engine is bundled: `WKWebView` is the system WebKit.
- Set the bundle id (`dev.pinback.shell`) and your signing team in Signing &
  Capabilities before running on a device.
