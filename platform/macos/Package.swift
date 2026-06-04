// swift-tools-version: 6.1
// Pinback macOS shell — pure Objective-C (AppKit + WebKit) for the smallest
// binary. Builds with `swift build`; no Swift sources, so it compiles on any
// recent Xcode (unlike the iOS SwiftUI WebView, which needs the iOS 26 SDK).
import PackageDescription

let package = Package(
    name: "PinbackShell",
    platforms: [
        // ObjC + WKWebView runs far back; v13 keeps the binary buildable on stock
        // CI runners while still running on the latest macOS.
        .macOS(.v13)
    ],
    targets: [
        .executableTarget(
            name: "PinbackShell",
            path: "Sources/PinbackShell",
            cSettings: [
                .unsafeFlags(["-fobjc-arc", "-Os"])
            ],
            linkerSettings: [
                .linkedFramework("Cocoa"),
                .linkedFramework("WebKit"),
                .unsafeFlags(["-Wl,-dead_strip"])
            ]
        )
    ]
)
