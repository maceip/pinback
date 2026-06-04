// swift-tools-version: 6.1
// Pinback macOS shell — a native window wrapping WKWebView via the new
// SwiftUI `WebView` API (macOS 26 "Tahoe").
import PackageDescription

let package = Package(
    name: "PinbackShell",
    platforms: [
        // The declarative SwiftUI WebView / WebPage API ships in macOS 26.
        .macOS("26.0")
    ],
    targets: [
        .executableTarget(
            name: "PinbackShell",
            path: "Sources/PinbackShell",
            // Optimize the release binary for size and strip dead code. The
            // WKWebView engine + Swift runtime are OS dylibs, never in the binary.
            swiftSettings: [
                .unsafeFlags(["-Osize"], .when(configuration: .release))
            ],
            linkerSettings: [
                .unsafeFlags(["-Wl,-dead_strip"])
            ]
        )
    ]
)
