# macOS shell — SwiftUI `WebView` (WKWebView)

A native macOS window hosting `WKWebView` through the **new declarative SwiftUI
`WebView` + `WebPage` API** (WebKit-for-SwiftUI, macOS 26 "Tahoe", currently beta
as of Xcode 26). This replaces the old `NSViewRepresentable` boilerplate — the
whole shell is ~25 lines.

## Layout

```
macos/
├── Package.swift                         # SwiftPM executable, macOS 26 platform
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

`swift run` produces a runnable AppKit/SwiftUI app. To ship a proper `.app`
bundle with an icon/entitlements, open `Package.swift` in Xcode 26 and archive,
or wrap the built binary in a minimal bundle.

## Notes

- No engine is bundled: `WKWebView` is the system WebKit.
- `WebPage` is an `@Observable` model — bind `page.url`, `page.title`,
  `page.estimatedProgress`, etc. for navigation chrome if you ever want more than
  a bare shell.
