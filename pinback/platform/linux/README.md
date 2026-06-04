# Linux shell — WebKitGTK (system engine, GTK 4)

A GTK 4 window whose only child is a `WebKitWebView`. The shell is ~30 lines of C.

## Why WebKitGTK (the "don't bundle the engine" decision)

Linux has no single OS-provided webview, so the choice is: **bundle a browser
engine** (Qt WebEngine / CEF / Ultralight — each ships a private Chromium, adding
100s of MB and making *you* responsible for engine CVEs) **or depend on the
distro-provided engine**. We do the latter:

`webkitgtk-6.0` (the GTK 4 / WebKit2 API) is packaged by every major distro and
linked dynamically via `pkg-config`, so the engine lives in the system, not in
this binary. This is the same engine Tauri's `wry` and the `webview/webview` C
library use on Linux. (A future bundle-free alternative is **Servo/`libservo`**,
but it is not yet a stable system library on any distro.)

## Layout

```
linux/
├── main.c          # the entire shell
├── meson.build     # gtk4 + webkitgtk-6.0 via pkg-config
└── README.md
```

## Dependencies (dev packages)

| Distro        | Packages                                   |
|---------------|--------------------------------------------|
| Debian/Ubuntu | `libgtk-4-dev libwebkitgtk-6.0-dev`        |
| Fedora        | `gtk4-devel webkitgtk6.0-devel`            |
| Arch          | `gtk4 webkitgtk-6.0`                        |

> This build host had neither GTK 4 nor WebKitGTK dev packages installed
> (`pkg-config --exists webkitgtk-6.0` → no), so the project is scaffolded but
> not compiled here.

## How it loads pinback

The shell self-hosts the cockpit: unless `PINBACK_URL` is set, it `fork`+`exec`s
`pinback-server` on `127.0.0.1:8088`, polls `GET /healthz` until `200`, loads it,
and `SIGTERM`s the server when the window closes. Build `pinback-server` from the
repo root (`make pinback-server`) and put it on `PATH` (or set
`PINBACK_SERVER_BIN` to its path). Set `PINBACK_URL` to skip spawning and load a
remote/dev server instead.

## Build & run

```sh
cd platform/linux
meson setup build
meson compile -C build
./build/pinback-shell                              # self-hosts pinback-server
PINBACK_URL=http://127.0.0.1:8088 ./build/pinback-shell   # or load your own
```

No Meson? A single command works too:

```sh
cc main.c -o pinback-shell $(pkg-config --cflags --libs gtk4 webkitgtk-6.0)
```
