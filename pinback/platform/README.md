# platform/

Platform-specific native **shells**: tiny, independent projects whose only job is
to open a native window and host the OS-provided webview engine pointed at the
Pinback cockpit UI served by `pinback-server` (the single C binary in this repo,
which embeds the web UI directly — there is no separate JS dev server).

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

## Shared runtime contract — how a shell "loads pinback"

The cockpit is `pinback-server`: a single binary that serves the embedded UI on
loopback and supervises the `ds4-agent` child. The shells split into two roles
depending on whether the platform can host that server.

**Desktop (macOS, Linux, Windows) — self-hosting.** The shell *is* the launcher.
At startup, unless `PINBACK_URL` is set, it spawns a bundled `pinback-server` on
`127.0.0.1:8088`, polls `GET /healthz` until it returns `200`, loads
`http://127.0.0.1:8088`, and terminates the server when the window closes. A
novice double-clicks one app — no terminal, no separate server to start. The
shell finds the server next to its own executable first, then a `PATH` install
(`PINBACK_SERVER_BIN` overrides the location). On Windows, `pinback-server` is
POSIX (pthreads/BSD sockets), so a native `.exe` isn't built by this repo's
`make`; the Windows shell still spawns `pinback-server.exe` if one is placed next
to it (e.g. an MSYS2/WSL build), otherwise point `PINBACK_URL` at a remote/WSL
server.

**Mobile (iOS, Android) — thin client.** A phone can't host the 87 GB-model
`ds4-agent`, so these shells load a **remote** pinback over the network. The URL
comes from `PINBACK_URL` (iOS Run-scheme env / Android `buildConfigField`) and
defaults to a loopback alias that reaches a server on your dev machine:

```
http://127.0.0.1:8088      # iOS Simulator (shares the Mac's network stack)
http://10.0.2.2:8088       # Android emulator (host loopback alias)
```

On a physical device, override `PINBACK_URL` with your machine's LAN / Tailscale
address. In every shell, setting `PINBACK_URL` is also the desktop escape hatch:
when set, the shell loads it verbatim and spawns nothing (point it at a server
you run yourself, e.g. `pinback-server --bind 127.0.0.1:8088`).

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

## Continuous integration

`.github/workflows/platform-shells.yml` compiles the **Linux**, **Windows**, and
**Android** shells on native GitHub runners, so they get real build coverage
without a local toolchain. **macOS** and **iOS** are not built in CI: they need
the macOS 26 / iOS 26 SDK (Xcode 26 beta) for the new SwiftUI `WebView`, which
hosted runners don't ship yet — build those locally with Xcode 26.

See each subdirectory's `README.md` for build/run instructions and the exact
newest-library version notes, and [`SIZE.md`](./SIZE.md) for the size research:
ranked smallest-code/smallest-binary approaches per platform, what's already
applied, and the remaining language/architecture forks.
