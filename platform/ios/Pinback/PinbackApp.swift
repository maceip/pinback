import SwiftUI
import WebKit

// The entire iOS shell. `WebView` + `WebPage` are the native SwiftUI WebKit
// bindings introduced in iOS 26 — no UIViewRepresentable wrapper required.
//
// Embedding model: iOS cannot host pinback-server (no ds4-agent / 87GB Metal
// model on-device), so the phone is a thin client that loads a REMOTE pinback
// over the network. Point it at your cockpit host with PINBACK_URL (the Run
// scheme's environment on device; on the Simulator the default 127.0.0.1:8088
// reaches a pinback-server running on your Mac).
@main
struct PinbackApp: App {
    var body: some Scene {
        WindowGroup {
            ShellView()
        }
    }
}

private struct ShellView: View {
    @State private var page = WebPage()

    private var url: URL {
        URL(string: ProcessInfo.processInfo.environment["PINBACK_URL"]
            ?? "http://127.0.0.1:8088")!
    }

    var body: some View {
        WebView(page)
            .ignoresSafeArea()
            .task { page.load(URLRequest(url: url)) }
    }
}
