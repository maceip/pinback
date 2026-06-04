# platform/

Platform-specific native **shells**: tiny, independent projects whose only job is
to open a native window and host the OS-provided webview engine pointed at the
Pinback / DS4 Runtime Cockpit UI (`tools/ds4-agent-ui`).

The directory is named `platform/` after the convention used by respected C/C++
codebases for OS-specific code — e.g. Godot (`platform/{macos,ios,android,
linuxbsd,windows,web}`) and WebKit (`Source/WebCore/platform`). Each subdirectory
is a self-contained project with its own build system; nothing here is shared at
the code level on purpose, so each shell stays "maximally small".

## Design rules

1. **No bundled engine.** Every shell links the *system* webview, never a vendored
   browser engine. That keeps each binary tiny and lets the OS patch the engine.
2. **Newest APIs.** Each project uses the most modern (often beta) native webview
   binding available for its platform.
3. **Minimal glue.** The goal is the smallest amount of native/glue code that can
   show a window and a webview. Real apps add IPC, menus, etc.; these don't.

| Platform | Window toolkit | Webview engine | Binding (newest) | Build |
|----------|----------------|----------------|------------------|-------|
| macOS    | SwiftUI (AppKit) | WebKit / WKWebView | SwiftUI `WebView`+`WebPage` (macOS 26) | SwiftPM |
| iOS      | SwiftUI (UIKit)  | WebKit / WKWebView | SwiftUI `WebView`+`WebPage` (iOS 26) | Xcode |
| Android  | `ComponentActivity` | System WebView (Chromium) | `android.webkit.WebView` | Gradle |
| Linux    | GTK 4 | WebKitGTK (system `libwebkitgtk-6.0`) | `webkitgtk-6.0` | Meson |
| Windows  | Win32 | WebView2 (system Edge runtime) | WebView2 SDK | CMake |

## Shared runtime contract

Every shell loads a single URL and nothing else. The URL is read from the
`PINBACK_URL` environment variable (or a per-platform build constant) and defaults
to the cockpit dev/preview server:

```
http://127.0.0.1:18192     # desktop (macOS, Linux, Windows) and iOS simulator
http://10.0.2.2:18192      # Android emulator (host loopback alias)
```

Start the UI it points at with:

```sh
cd tools/ds4-agent-ui && bun install && bun run dev   # or: bun run preview
```

## Why these engines (and the Linux question)

The interesting decision is Linux, where there is no single "system webview".
Options considered:

- **WebKitGTK (`webkitgtk-6.0`, GTK 4)** — *chosen.* It is packaged by every major
  distro (`libwebkitgtk-6.0`, Fedora `webkitgtk6.0`, Arch `webkitgtk-6.0`) and
  linked dynamically via `pkg-config`, so the engine is **not** bundled. This is
  also what Tauri's `wry` and the `webview/webview` C library use under the hood.
- **Qt WebEngine / CEF / Ultralight** — rejected: each vendors a full Chromium (or
  custom) engine, ballooning the binary to 100s of MB and making you the one who
  ships engine security patches.
- **Servo (`servoshell`/`libservo`)** — interesting future option (embeddable,
  Rust), but not yet a stable system library on any distro, so it would have to be
  bundled today.

The net rule: on Linux you either bundle Chromium or you depend on the
distro-provided WebKitGTK. To honor "don't bundle the engine", we depend on
WebKitGTK.

See each subdirectory's `README.md` for build/run instructions and the exact
newest-library version notes.
