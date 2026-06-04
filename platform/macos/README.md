# macOS shell — AppKit + WKWebView (Objective-C)

A native macOS window hosting `WKWebView`, written in **pure Objective-C** for the
smallest binary. The window chrome and the WKWebView engine are the OS's, so this
paints identically to a SwiftUI shell — it just drops the Swift/SwiftUI codegen.
The whole shell is ~35 lines (`main.m`).

> Size-optimized variant. The new SwiftUI `WebView`/`WebPage` API (macOS 26) is
> the *newest* way to write this, but it produces a larger binary and requires the
> macOS 26 SDK. We chose minimum binary here; iOS keeps the SwiftUI variant.

## Layout

```
macos/
├── Package.swift                       # SwiftPM executable, ObjC target, macOS 13+
├── Scripts/bundle.sh                   # wrap the binary into Pinback.app (+ icon)
├── Resources/AppIcon.icns              # brand icon
└── Sources/PinbackShell/main.m         # the entire app
```

## Requirements

- Any recent Xcode / Swift 6.1 toolchain (no Xcode 26 needed — there's no Swift code).
- Builds and runs on macOS 13+.

## Build & run

```sh
cd platform/macos
swift run                                   # builds and launches the window
PINBACK_URL=http://127.0.0.1:18192 swift run
```

`swift run` produces a runnable AppKit app and is fine for quick iteration. But a
bare binary has no `Info.plist`, so App Transport Security blocks the plain-http
dev server. For a proper bundle (ATS allowing local networking, Dock icon):

```sh
./Scripts/bundle.sh        # builds release + assembles Pinback.app with AppIcon.icns
open Pinback.app
```

## Notes

- No engine is bundled: `WKWebView` is the system WebKit.
- Size flags: `-Os` + `-Wl,-dead_strip` (in `Package.swift`) and `strip -x`
  (in `bundle.sh`).
- Built in CI on `macos-latest` (a pure-ObjC target needs no beta SDK).
