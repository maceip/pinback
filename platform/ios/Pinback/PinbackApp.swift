import SwiftUI
import WebKit

@main
struct PinbackApp: App {
    var body: some Scene {
        WindowGroup {
            RootView()
        }
    }
}

private struct Workspace: Identifiable, Decodable, Hashable {
    let id: String
    let label: String
    let path: String
}

private struct BridgeState: Decodable {
    let activeId: String?
    let canGoBack: Bool?
    let workspaces: [Workspace]
}

private enum SessionPhase {
    case checking
    case setup
    case cockpit(URL)
}

private struct RootView: View {
    @AppStorage("pinbackServerURL") private var serverURL = ""
    @State private var phase: SessionPhase = .checking

    private var envURL: String? {
        let v = ProcessInfo.processInfo.environment["PINBACK_URL"]?.trimmingCharacters(in: .whitespacesAndNewlines)
        return (v?.isEmpty == false) ? v : nil
    }

    private var defaultURL: String {
        if let e = envURL { return e }
        #if targetEnvironment(simulator)
        return "http://127.0.0.1:8088"
        #else
        return ""
        #endif
    }

    var body: some View {
        Group {
            switch phase {
            case .checking:
                ProgressView("Connecting…")
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            case .setup:
                SetupWebView(
                    prefill: serverURL.isEmpty ? defaultURL : serverURL
                ) { url in
                    serverURL = url.absoluteString
                    phase = .cockpit(url)
                }
            case .cockpit(let url):
                ShellView(baseURL: url, onOpenSettings: { phase = .setup })
            }
        }
        .task { await boot() }
    }

    @MainActor
    private func boot() async {
        let candidate = envURL ?? (serverURL.isEmpty ? defaultURL : serverURL)
        guard !candidate.isEmpty, let url = URL(string: normalizeURL(candidate)) else {
            phase = .setup
            return
        }
        if await healthOK(url) {
            serverURL = url.absoluteString
            phase = .cockpit(url)
        } else {
            phase = .setup
        }
    }
}

// Bundled platform/common/setup.html + pinback-host.js (see platform/CONTRACT.md).
private struct SetupWebView: UIViewRepresentable {
    let prefill: String
    let onConnect: (URL) -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(prefill: prefill, onConnect: onConnect)
    }

    func makeUIView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        config.userContentController.add(context.coordinator, name: "pinbackSetup")
        let webView = WKWebView(frame: .zero, configuration: config)
        webView.navigationDelegate = context.coordinator
        context.coordinator.webView = webView
        if let url = Bundle.main.url(forResource: "setup", withExtension: "html", subdirectory: "HostAssets") {
            webView.loadFileURL(url, allowingReadAccessTo: url.deletingLastPathComponent())
        }
        return webView
    }

    func updateUIView(_ webView: WKWebView, context: Context) {}

    final class Coordinator: NSObject, WKNavigationDelegate, WKScriptMessageHandler {
        let prefill: String
        let onConnect: (URL) -> Void
        weak var webView: WKWebView?

        init(prefill: String, onConnect: @escaping (URL) -> Void) {
            self.prefill = prefill
            self.onConnect = onConnect
        }

        func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
            guard !prefill.isEmpty else { return }
            let esc = prefill
                .replacingOccurrences(of: "\\", with: "\\\\")
                .replacingOccurrences(of: "'", with: "\\'")
            webView.evaluateJavaScript(
                "document.getElementById('url').value='\(esc)';"
            ) { _, _ in }
        }

        func userContentController(
            _ userContentController: WKUserContentController,
            didReceive message: WKScriptMessage
        ) {
            guard message.name == "pinbackSetup" else { return }
            let body: [String: Any]
            if let dict = message.body as? [String: Any] {
                body = dict
            } else if let json = message.body as? String,
                      let data = json.data(using: .utf8),
                      let dict = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                body = dict
            } else {
                return
            }
            guard body["type"] as? String == "pinback-setup" else { return }
            let action = body["action"] as? String ?? ""
            guard let raw = body["url"] as? String,
                  let url = URL(string: normalizeURL(raw)) else { return }

            Task { @MainActor in
                let ok = await healthOK(url)
                switch action {
                case "test":
                    let js = ok
                        ? "document.getElementById('status').textContent='Server is reachable.';document.getElementById('status').className='ok';"
                        : "document.getElementById('status').textContent='Cannot reach server at that URL.';document.getElementById('status').className='err';"
                    webView?.evaluateJavaScript(js) { _, _ in }
                case "connect":
                    if ok {
                        onConnect(url)
                    } else {
                        webView?.evaluateJavaScript(
                            "document.getElementById('status').textContent='Saved, but server still unreachable.';document.getElementById('status').className='err';"
                        ) { _, _ in }
                    }
                default:
                    break
                }
            }
        }
    }
}

private struct ShellView: View {
    let baseURL: URL
    let onOpenSettings: () -> Void

    @State private var workspaces: [Workspace] = []
    @State private var activeId: String?
    @State private var selectedId: String?
    @State private var showInspector = false
    @State private var columnVisibility: NavigationSplitViewVisibility = .automatic
    @State private var webViewRef: WKWebView?

    private var active: Workspace? {
        workspaces.first { $0.id == activeId }
    }

    var body: some View {
        NavigationSplitView(columnVisibility: $columnVisibility) {
            List(workspaces, selection: $selectedId) { ws in
                VStack(alignment: .leading, spacing: 2) {
                    Text(ws.label.isEmpty ? ws.id : ws.label)
                    if !ws.path.isEmpty {
                        Text(ws.path)
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                            .truncationMode(.head)
                    }
                }
            }
            .navigationTitle("Workspaces")
        } detail: {
            CockpitWebView(
                baseURL: baseURL,
                onOpenSettings: onOpenSettings,
                webViewRef: $webViewRef
            )
            .ignoresSafeArea()
            .navigationTitle(active.map { $0.label.isEmpty ? $0.id : $0.label } ?? "Pinback")
            .toolbar {
                ToolbarItem {
                    Button("Reload", systemImage: "arrow.clockwise") {
                        webViewRef?.load(URLRequest(url: baseURL))
                    }
                }
                ToolbarSpacer(.flexible)
                ToolbarItem {
                    Button("Server", systemImage: "network") { onOpenSettings() }
                }
                ToolbarItem {
                    ShareLink(item: baseURL)
                }
                ToolbarSpacer(.fixed)
                ToolbarItem {
                    Button("Info", systemImage: "info.circle") { showInspector.toggle() }
                }
            }
            .inspector(isPresented: $showInspector) {
                InspectorView(active: active, count: workspaces.count)
                    .inspectorColumnWidth(min: 220, ideal: 280, max: 360)
            }
        }
        .task {
            while !Task.isCancelled {
                await pollState()
                try? await Task.sleep(for: .seconds(1.5))
            }
        }
        .onChange(of: selectedId) { _, newValue in
            guard let id = newValue, id != activeId else { return }
            webViewRef?.evaluateJavaScript(
                "window.pinback && window.pinback.selectWorkspace('\(jsEscape(id))');"
            )
        }
    }

    @MainActor
    private func pollState() async {
        guard let webViewRef else { return }
        let result = try? await webViewRef.evaluateJavaScript(
            "JSON.stringify(window.pinback ? window.pinback.state() : null)"
        )
        guard
            let json = result as? String, json != "null",
            let data = json.data(using: .utf8),
            let state = try? JSONDecoder().decode(BridgeState.self, from: data)
        else { return }

        workspaces = state.workspaces
        activeId = state.activeId
        if selectedId != state.activeId { selectedId = state.activeId }
    }
}

private struct CockpitWebView: UIViewRepresentable {
    let baseURL: URL
    let onOpenSettings: () -> Void
    @Binding var webViewRef: WKWebView?

    func makeCoordinator() -> Coordinator {
        Coordinator(onOpenSettings: onOpenSettings)
    }

    func makeUIView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        config.userContentController.add(context.coordinator, name: "pinback")
        let webView = WKWebView(frame: .zero, configuration: config)
        context.coordinator.webView = webView
        webView.load(URLRequest(url: baseURL))
        DispatchQueue.main.async { webViewRef = webView }
        return webView
    }

    func updateUIView(_ webView: WKWebView, context: Context) {}

    final class Coordinator: NSObject, WKScriptMessageHandler {
        let onOpenSettings: () -> Void
        weak var webView: WKWebView?

        init(onOpenSettings: @escaping () -> Void) {
            self.onOpenSettings = onOpenSettings
        }

        func userContentController(
            _ userContentController: WKUserContentController,
            didReceive message: WKScriptMessage
        ) {
            guard message.name == "pinback",
                  let body = message.body as? [String: Any],
                  body["type"] as? String == "pinback-host",
                  body["action"] as? String == "openSetup" else { return }
            DispatchQueue.main.async { self.onOpenSettings() }
        }
    }
}

private struct InspectorView: View {
    let active: Workspace?
    let count: Int

    var body: some View {
        Form {
            Section("Active workspace") {
                LabeledContent("Name", value: active.map { $0.label.isEmpty ? $0.id : $0.label } ?? "—")
                if let path = active?.path, !path.isEmpty {
                    LabeledContent("Path") {
                        Text(path).font(.system(.footnote, design: .monospaced))
                    }
                }
            }
            Section {
                LabeledContent("Workspaces", value: "\(count)")
            }
        }
    }
}

private func normalizeURL(_ raw: String) -> String {
    var s = raw.trimmingCharacters(in: .whitespacesAndNewlines)
    if s.isEmpty { return s }
    let lower = s.lowercased()
    if !lower.hasPrefix("http://") && !lower.hasPrefix("https://") {
        s = "http://" + s
    }
    while s.hasSuffix("/") { s.removeLast() }
    return s
}

private func jsEscape(_ s: String) -> String {
    s.replacingOccurrences(of: "\\", with: "\\\\")
        .replacingOccurrences(of: "'", with: "\\'")
}

private func healthOK(_ base: URL) async -> Bool {
    let health = base.appendingPathComponent("healthz")
    var req = URLRequest(url: health)
    req.timeoutInterval = 4
    do {
        let (_, resp) = try await URLSession.shared.data(for: req)
        return (resp as? HTTPURLResponse)?.statusCode == 200
    } catch {
        return false
    }
}
