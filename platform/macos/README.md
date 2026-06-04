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

## Run

```sh
cd platform/macos
swift run                                  # builds and launches the window
PINBACK_URL=http://127.0.0.1:18192 swift run
```

`swift run` produces a runnable AppKit/SwiftUI app and is fine for quick
iteration. But a bare binary has no `Info.plist`, so App Transport Security will
block the plain-http dev server. To get a proper bundle (with ATS allowing local
networking):

```sh
./Scripts/bundle.sh        # builds release + assembles Pinback.app
open Pinback.app
```

For an icon/entitlements/signing, open `Package.swift` in Xcode 26 and archive.

## Notes

- No engine is bundled: `WKWebView` is the system WebKit.
- `WebPage` is an `@Observable` model — bind `page.url`, `page.title`,
  `page.estimatedProgress`, etc. for navigation chrome if you ever want more than
  a bare shell.
