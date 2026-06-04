import SwiftUI
import WebKit

// The entire macOS shell. `WebView` + `WebPage` are the native SwiftUI WebKit
// bindings introduced in macOS 26 — no NSViewRepresentable wrapper required.
//
// Embedding model (how the shell "loads pinback"):
//   - If PINBACK_URL is set, load it verbatim and spawn nothing. This is the
//     dev / remote escape hatch (point at a server you run yourself).
//   - Otherwise the shell IS the launcher: it spawns the bundled
//     `pinback-server` child on 127.0.0.1:8088, waits for /healthz, then loads
//     it. The server is terminated when the app quits. This makes Pinback.app a
//     self-contained, double-click-to-run cockpit for a novice — no terminal.
@main
struct PinbackShellApp: App {
    @State private var server = PinbackServer()

    var body: some Scene {
        WindowGroup("Pinback") {
            ShellView(server: server)
                .onDisappear { server.stop() }
        }
        .defaultSize(width: 1280, height: 800)
    }
}

private struct ShellView: View {
    let server: PinbackServer
    @State private var page = WebPage()

    var body: some View {
        WebView(page)
            .ignoresSafeArea()
            .task {
                let url = await server.start()
                page.load(URLRequest(url: url))
            }
    }
}
