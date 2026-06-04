# Android shell — `android.webkit.WebView`

A single-Activity app whose content view *is* the system `WebView` (Chromium,
updated by the OS / Play Store), pointed at the cockpit UI. No layout XML, no
fragments, no Compose — the whole shell is `MainActivity.kt`.

## Layout

```
android/
├── settings.gradle.kts
├── build.gradle.kts
├── gradle.properties
├── gradle/libs.versions.toml          # version catalog (AGP 8.13, Kotlin 2.2)
├── gradlew / gradlew.bat / gradle/wrapper/   # Gradle 8.14.3 wrapper (checked in)
└── app/
    ├── build.gradle.kts
    └── src/main/
        ├── AndroidManifest.xml
        └── kotlin/com/pinback/shell/MainActivity.kt
```

## Toolchain

- Android Gradle Plugin **8.13.0**, Gradle **8.14.3** (wrapper checked in), Kotlin **2.2**.
- `compileSdk`/`targetSdk` **36** (Android 16), `minSdk` 24.
- JDK 17+ (this repo's CI host has JDK 21).
- Requires an installed Android SDK (`ANDROID_HOME` / `local.properties`). This
  build host did **not** ship the SDK, so the project is scaffolded but not built
  here.

> Scaffolding note: the brief mentioned a "new Android CLI" on this machine, but
> no Android SDK/CLI (`sdkmanager`, `avdmanager`, the Studio `android` tool) was
> present — only Gradle + JDK. This project mirrors exactly what those tools emit
> (version catalog, Kotlin DSL, wrapper) so `./gradlew` works as soon as an SDK is
> available.

## Build & run

```sh
cd platform/android
./gradlew assembleDebug                 # builds app/build/outputs/apk/debug/app-debug.apk
./gradlew installDebug                  # install to a running emulator/device
```

On the emulator the default URL is `http://10.0.2.2:18192` — `10.0.2.2` is the
emulator's alias for the host loopback, so it reaches the cockpit dev server on
your machine. `res/xml/network_security_config.xml` permits plain http only to
the loopback hosts (everything else stays https-only). Override the URL at build
time by editing the `PINBACK_URL` `buildConfigField`.

## Notes

- No engine is bundled: `WebView` uses the device's system WebView/Chrome.
- `WebViewClient()` keeps navigations inside the shell instead of launching a
  browser.
