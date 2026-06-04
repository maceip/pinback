# Minimizing the shells (size research)

Goal: the smallest amount of glue code and the smallest binary that embeds the
**system** webview. None of the shells bundle an engine, so the engine itself
costs **0 bytes** in every binary — only our glue + loader stubs count. That
reframes the whole exercise: every option here is **KB-to-low-MB**, and the only
way to "save megabytes" would be to bundle Chromium (Electron: 100–500 MB),
which we explicitly don't.

## Framework baselines (empty app, system-webview frameworks)

| | Windows | macOS | Linux | Engine |
|---|---|---|---|---|
| **Our hand-written shells** | **tens of KB** | tens–hundreds of KB | tens of KB | system |
| Neutralino | ~1 MB | ~1 MB | ~1 MB | system |
| Tauri (wry/tao) | ~3 MB | ~4 MB | ~3 MB | system |
| Wails (Go) | ~11 MB | ~8 MB | ~8 MB | system |
| Electron | 300–540 MB | 300–400 MB | 300–600 MB | **bundled** |

Hand-written native + system webview is the floor — below every framework,
because frameworks add an IPC/JS-bridge runtime we don't need. Reference:
[Elanis/web-to-desktop-framework-comparison](https://github.com/Elanis/web-to-desktop-framework-comparison),
[webview/webview](https://github.com/webview/webview).

## Per-platform findings & ranked approaches (smallest first)

### Linux — `webkitgtk-6.0` + GTK4 (current) ✅ already optimal
WebKitGTK is the **only** practical non-bundled system webview on Linux (WebKit-EFL
is dead; WPE is embedded/kiosk-only; Qt/CEF/Ultralight bundle Chromium). Plain C +
GTK4 is the smallest *code* and smallest *binary* (engine is external). **Applied:**
`buildtype=minsize` + LTO + `-ffunction-sections/-fdata-sections` +
`-Wl,--gc-sections,--as-needed -s`. Nothing leaner exists.
Sources: [WebKitGTK API versions](https://blogs.gnome.org/mcatanzaro/2025/04/28/webkitgtk-api-versions/),
[rosenrot (256-line C browser)](https://github.com/NunoSempere/rosenrot-browser).

### Windows — Win32 + WebView2
1. **no-CRT + built-in loader + raw COM** — ~few KB exe, ships only the .exe.
2. **dynamic CRT + built-in loader + raw COM** — ~10–30 KB, no DLL, no import lib. *(sweet spot)*
3. dynamic CRT + static loader + WRL — no DLL, but pulls extra libs.
4. **current: dynamic CRT + loader DLL + WRL** — must ship `WebView2Loader.dll`.
- ⚠️ **Pure C is a trap**: the `ICoreWebView2EnvironmentOptions` default impl is
  C++-only and hand-rolled C vtables render nothing ([#1124](https://github.com/MicrosoftEdge/WebView2Feedback/issues/1124)).
- The big win is **eliminating the loader** (registry lookup → `LoadLibraryW` →
  `GetProcAddress("CreateWebViewEnvironmentWithOptionsInternal")`), exactly what
  [webview/webview](https://github.com/webview/webview) and
  [jchv/OpenWebView2Loader](https://github.com/jchv/OpenWebView2Loader) do.
- **Applied (safe):** `/O1 /Os /Gy` + `/OPT:REF /OPT:ICF`, dynamic CRT kept
  (`/MT` is ~10× bigger — [minimal executables](https://scorpiosoftware.net/2023/03/16/minimal-executables/)).

### Android — `android.webkit.WebView`
The system WebView (Chromium) is a separate OS package → **0 bytes** in the APK.
"Smallest WebView APK" = "smallest one-Activity APK".
1. **Java + plain `android.app.Activity` + no AndroidX + no `res/` + R8** — ~10–40 KB
   (hand-built floor is [2,871 bytes](https://github.com/krossovochkin/SmallestAPK)).
2. Kotlin + plain Activity + no AndroidX + R8 — +~15–20 KB (stdlib residual).
3. **current: Kotlin + androidx.activity + R8** — hundreds of KB (the AndroidX
   graph dominates; [issuetracker 161814404](https://issuetracker.google.com/issues/161814404)).
- **Applied (safe):** R8 full-mode `isMinifyEnabled` + `isShrinkResources`.
- The order-of-magnitude win requires **dropping AndroidX** (replace
  `enableEdgeToEdge()` with a one-line window flag) — a language/API fork, see below.

### macOS — WKWebView
1. **Pure Objective-C** (AppKit + WebKit, ~45 lines, no nib) — **~10–30 KB**.
2. Pure C via `objc_msgSend` — same size, 2–4× the code (only worth it for a
   single-header *library*, e.g. webview/webview's `cocoa_webkit.hh`).
3. **current: SwiftUI `WebView`/`WebPage` (macOS 26)** — ~100 KB, fewest lines (29);
   Swift runtime is OS-provided since macOS 10.14.4 so it's **not** in the binary.
- **Applied (safe):** `-Osize` + `-Wl,-dead_strip` in `Package.swift`, `strip -x`
  in `bundle.sh`.
- ObjC is the smaller *binary*; SwiftUI is the smaller *line count* + newest API.
Sources: [Cocoa-with-Love minimal (9 KB)](https://www.cocoawithlove.com/2010/09/minimalist-cocoa-programming.html),
[Swift ABI stability](https://www.swift.org/blog/abi-stability-and-apple/).

### iOS — WKWebView
1. **Single-file Objective-C `main.m`, no pbxproj** (`xcrun clang` + `codesign`) —
   smallest project *and* binary; saves ~1–2 MB vs SwiftUI.
2. **Single-file ObjC inside the current synchronized-group pbxproj** — keeps
   `xcodebuild`, drops ~1–2 MB of Swift metadata.
3. **current: SwiftUI `WebView` (iOS 26)** — ~1–2 MB, fewest lines, newest API; on
   iOS 12.2+ the Swift runtime is in the OS (no bundled dylibs).
- **Applied (safe):** `DEAD_CODE_STRIPPING = YES` + `SWIFT_OPTIMIZATION_LEVEL =
  -Osize` (Release). Keep the empty `UILaunchScreen` dict (required for native
  fullscreen); the app icon is only needed for App Store submission.
Sources: [Swift runtime back-deployment](https://milen.me/writings/apple-link-magic-swift-runtime/),
[iOS app-size drivers](https://docs.emergetools.com/docs/ios-app-size).

## The genuine forks (need a build to measure; decide before/at teleport)

| Platform | Newest-API (current) | Absolute-smallest | Delta |
|---|---|---|---|
| macOS | SwiftUI WebView, 29 ln, ~100 KB | pure ObjC, ~45 ln, ~10–30 KB | ~70 KB |
| iOS | SwiftUI WebView, ~1–2 MB | single-file ObjC `main.m` | ~1–2 MB |
| Android | Kotlin + AndroidX, 100s KB | Java, no-AndroidX, ~10–40 KB | order of magnitude |
| Windows | WRL + loader DLL | raw-COM + built-in loader, single exe | drops the DLL |

The "current" column keeps the newest/beta APIs (the original brief); the
"smallest" column trades those for the minimum binary. All four are best verified
by actually building + measuring (`bloaty`/`xcrun size`/`ls -l`) on the target
machine — which is the plan once teleported.

## Trade-offs: what each smaller variant gives up (keeping native/modern paint)

Measured on a Windows build host (VS 18 / MSVC 19.50, Android SDK 36):

| Platform | Current (modern) — measured | Smallest variant | What you lose | Hurts native/modern *paint*? |
|---|---|---|---|---|
| **Windows** | WRL + loader DLL — **21 KB exe + 159 KB `WebView2Loader.dll`** (verified rendering) | raw-COM + built-in loader → **single ~25 KB exe**, no DLL | WRL ergonomics (more verbose COM) | **No** — same Win32 window, same WebView2 engine |
| **Android** | Kotlin + AndroidX — **80 KB** release APK (R8, verified rendering) | Java + no-AndroidX plain `Activity` → ~10–40 KB | `enableEdgeToEdge()` insets, predictive-back, Kotlin ergonomics | **Yes (some)** — edge-to-edge + predictive back are modern *paint/UX* |
| **macOS** | SwiftUI `WebView` (macOS 26) ~100 KB | pure ObjC AppKit ~10–30 KB | newest SwiftUI API; needs macOS 26 either way | **No** — same NSWindow + WKWebView |
| **iOS** | SwiftUI `WebView` (iOS 26) ~1–2 MB | single-file ObjC ~hundreds KB | newest SwiftUI API; auto safe-area handling | **No** (with trivial manual safe-area) |

**The load-bearing insight:** every shell paints with the OS's native window + the
OS's webview engine, so **none of the smaller variants change the rendered output.**
What they trade away is *API modernity / developer ergonomics* — and, on Android
only, two genuine modern-*paint* behaviours (edge-to-edge insets and predictive
back) that are tied to AndroidX.

**Recommendation for "keep native/modern paint, stay small":**
- **Windows** — adopt the built-in loader. Single-file, zero paint cost; the only
  downside is more verbose COM. (Skip the no-CRT ~4 KB build — fragile, no paint gain.)
- **Android** — **keep** Kotlin + AndroidX. This is the one place dropping deps costs
  modern paint/UX, and 80 KB is already almost entirely glue (the WebView engine is
  a system package = 0 bytes in the APK). Saving ~50 KB isn't worth losing edge-to-edge.
- **macOS / iOS** — **keep** SwiftUI `WebView` *if* macOS 26 / iOS 26 minimums are
  acceptable (most modern, paint-identical). Switch to Objective-C only if you need
  older-OS support: paint stays identical, you gain broad support + a smaller binary,
  you lose the newest SwiftUI API. Not a paint decision — an OS-floor decision.

## Decisions & results (implemented)

Direction chosen: **prefer small binary over small code; optimize size on macOS +
Windows; keep the OS frameworks on Android + iOS** (the framework-only features
like predictive back aren't worth hand-rolling); target latest OS only, so the
"backport to old OS" benefit of the frameworks is moot.

| Platform | Decision | Result |
|---|---|---|
| **Windows** | **built-in loader** (no `WebView2Loader.dll`), WRL kept, size flags | ✅ built + run-verified on this box: **single self-contained exe**, no DLL; **~151 KB** (only ~23 KB code; ~128 KB is the embedded brand icon). Footprint ~310 KB→151 KB and 2 files→1. |
| **macOS** | **pure Objective-C** (AppKit + WKWebView), drop SwiftUI | ✅ rewritten; builds via `swift build` on `macos-latest` in CI (no beta SDK needed). Paint-identical to SwiftUI. |
| **Android** | **keep** Kotlin + AndroidX (edge-to-edge + predictive back come free) | ✅ unchanged; **80 KB** R8 release APK, render-verified on emulator. |
| **iOS** | **keep** SwiftUI `WebView` (newest API, OS-provided runtime) | unchanged; builds with Xcode 26 (not in CI). |
| **Linux** | get it working first (size already minimal) | needs a Linux build to verify — covered by the CI `linux` job; not buildable on the Windows host. |

CI now builds **Linux, Windows, Android, and macOS** on stock runners; only iOS is
excluded (needs the Xcode 26 SDK for the SwiftUI `WebView`).

Net: the one purely-free size cut (no paint/UX loss) was **Windows → built-in
loader**, now shipped and verified. macOS went ObjC for a smaller binary with
identical paint. Android + iOS keep their frameworks because the modern paint/UX
there *is* the framework, and their footprints are already tiny.
