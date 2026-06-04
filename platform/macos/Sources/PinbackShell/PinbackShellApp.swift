import SwiftUI
import WebKit

// The entire macOS shell. `WebView` + `WebPage` are the native SwiftUI WebKit
// bindings introduced in macOS 26 — no NSViewRepresentable wrapper required.
@main
struct PinbackShellApp: App {
    var body: some Scene {
        WindowGroup("Pinback") {
            ShellView()
        }
        .defaultSize(width: 1280, height: 800)
    }
}

private struct ShellView: View {
    @State private var page = WebPage()

    private var url: URL {
        URL(string: ProcessInfo.processInfo.environment["PINBACK_URL"]
            ?? "http://127.0.0.1:18192")!
    }

    var body: some View {
        WebView(page)
            .ignoresSafeArea()
            .task { page.load(URLRequest(url: url)) }
    }
}
