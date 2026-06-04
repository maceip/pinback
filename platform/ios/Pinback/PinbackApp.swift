import SwiftUI
import WebKit

// The entire iOS shell. `WebView` + `WebPage` are the native SwiftUI WebKit
// bindings introduced in iOS 26 — no UIViewRepresentable wrapper required.
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
            ?? "http://127.0.0.1:18192")!
    }

    var body: some View {
        WebView(page)
            .ignoresSafeArea()
            .task { page.load(URLRequest(url: url)) }
    }
}
