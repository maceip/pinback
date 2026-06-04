# Windows shell — WebView2 (system Edge runtime)

A Win32 window hosting a WebView2 control. WebView2 is rendered by the
system-installed **Microsoft Edge WebView2 "Evergreen" Runtime** (Chromium), so
the engine is not bundled. This build also ships **no `WebView2Loader.dll`**: it
locates the runtime via the registry and calls its internal environment entry
point directly (the technique the `webview/webview` library uses), so the
deliverable is a **single self-contained exe**. WRL (Windows SDK, header-only) is
kept for the async COM callbacks.

## Layout

```
windows/
├── main.cpp          # the shell + built-in loader (~140 lines)
├── app.manifest      # per-monitor-v2 DPI + UTF-8 active code page
├── app.rc / app.ico  # embedded brand icon (window + taskbar + Explorer)
├── CMakeLists.txt    # fetches WebView2 SDK *headers* from NuGet (no loader linked)
├── build.bat         # one-shot configure + build convenience wrapper
└── README.md
```

## Requirements

- Windows 10/11 with the [WebView2 Evergreen Runtime](https://developer.microsoft.com/microsoft-edge/webview2/)
  (preinstalled on current Windows).
- MSVC (VS 2022/2026 Build Tools) + CMake ≥ 3.24. Network at configure time to
  fetch the `Microsoft.Web.WebView2` NuGet package for its headers.

## Build & run

```bat
cd platform\windows
build.bat                          REM or the explicit commands below
```

```bat
cmake -S . -B build -A x64         REM -A x86 or -A arm64 also supported
cmake --build build --config Release
set PINBACK_URL=http://127.0.0.1:18192
build\Release\pinback-shell.exe
```

## Verified result (this machine)

Built with MSVC 19.50 / VS 18, CMake 4.3:

- **Single self-contained exe, no DLL** — runs from a clean directory with no
  `WebView2Loader.dll` present; spawns the WebView2 render processes and displays
  the page. Footprint dropped from ~310 KB (icon-bearing exe + 159 KB loader DLL,
  two files) to **~151 KB, one file** — of which only **~23 KB is code**; the rest
  is the embedded 256-px brand icon (`app.ico`, 128 KB).

## Notes

- No engine is bundled; WebView2 uses the system Edge runtime.
- The built-in loader reads `HKLM\…\EdgeUpdate\ClientState\{F301…}` → `EBWebView`
  to find the runtime, then `LoadLibraryW` + `GetProcAddress` the internal
  `CreateWebViewEnvironmentWithOptionsInternal`. Falls back to a clear message box
  if the runtime is missing.
- Architecture auto-selected at compile time (`x64`/`x86`/`arm64`).
- To track newer SDK headers: `cmake -S . -B build -DWEBVIEW2_VERSION=<x.y.z>`.
- To shrink the exe further, generate `app.ico` without the 256-px frame (saves
  ~90 KB) — at the cost of the high-DPI taskbar/jumbo icon.
