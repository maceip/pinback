# Windows shell — WebView2 (system Edge runtime)

A Win32 window hosting a WebView2 control. WebView2 is rendered by the
system-installed **Microsoft Edge WebView2 "Evergreen" Runtime** (Chromium),
which Windows keeps updated — so the engine is not bundled. The only glue is
`<wrl.h>` from the Windows SDK for the async COM callbacks.

## Layout

```
windows/
├── main.cpp          # the entire shell (~90 lines)
├── app.manifest      # per-monitor-v2 DPI + UTF-8 active code page
├── CMakeLists.txt    # fetches the WebView2 SDK from NuGet via FetchContent
├── build.bat         # one-shot configure + build convenience wrapper
└── README.md
```

## Requirements

- Windows 10/11 with the [WebView2 Evergreen Runtime](https://developer.microsoft.com/microsoft-edge/webview2/)
  (preinstalled on current Windows; otherwise install the bootstrapper).
- MSVC (Visual Studio 2022 Build Tools) + CMake ≥ 3.24. Network access at
  configure time to download the `Microsoft.Web.WebView2` NuGet package
  (default `1.0.3967.48`, the latest stable SDK).

> This build host is Linux without MSVC, so the project is scaffolded but not
> compiled here.

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

## Notes

- No engine is bundled; WebView2 uses the system Edge runtime.
- **Loader**: by default CMake links the import lib and copies `WebView2Loader.dll`
  next to the exe (reliable across CRT settings). For a single-file deploy with no
  DLL, configure with `-DWEBVIEW2_STATIC_LOADER=ON` to link
  `WebView2LoaderStatic.lib` instead.
- Architecture is auto-selected from `-A` (`x64` default, also `x86`/`arm64`).
- Default URL is `http://127.0.0.1:18192` (the cockpit dev server on this
  machine); override with the `PINBACK_URL` environment variable.
- To track a newer SDK: `cmake -S . -B build -DWEBVIEW2_VERSION=<x.y.z>`.
