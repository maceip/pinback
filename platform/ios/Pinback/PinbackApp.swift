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

    private var defaultURL: String {
        ProcessInfo.processInfo.environment["PINBACK_URL"] ?? "http://127.0.0.1:8088"
    }

    var body: some View {
        Group {
            switch phase {
            case .checking:
                ProgressView("Connecting…")
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            case .setup:
                ConnectView(
                    initialURL: serverURL.isEmpty ? defaultURL : serverURL,
                    onConnect: { url in
                        serverURL = url.absoluteString
                        phase = .cockpit(url)
                    }
                )
            case .cockpit(let url):
                ShellView(baseURL: url, onOpenSettings: {
                    phase = .setup
                })
            }
        }
        .task {
            await boot()
        }
    }

    @MainActor
    private func boot() async {
        let candidate = serverURL.isEmpty ? defaultURL : serverURL
        guard let url = URL(string: candidate) else {
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

private struct ConnectView: View {
    @State private var urlText: String
    @State private var status = "Enter the pinback-server URL on your Mac or PC."
    @State private var statusOK = false
    let onConnect: (URL) -> Void

    init(initialURL: String, onConnect: @escaping (URL) -> Void) {
        _urlText = State(initialValue: initialURL)
        self.onConnect = onConnect
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Server URL", text: $urlText)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .keyboardType(.URL)
                } header: {
                    Text("Pinback server")
                } footer: {
                    Text("Simulator: use http://127.0.0.1:8088 with pinback-server on your Mac. Physical device: use your Mac’s LAN or Tailscale address.")
                }
                Section {
                    Button("Test connection") {
                        Task { await test() }
                    }
                    Button("Connect") {
                        Task { await connect() }
                    }
                    .buttonStyle(.borderedProminent)
                }
                if !status.isEmpty {
                    Section {
                        Text(status)
                            .foregroundStyle(statusOK ? .green : .secondary)
                    }
                }
            }
            .navigationTitle("Connect")
        }
    }

    private func normalized() -> URL? {
        var s = urlText.trimmingCharacters(in: .whitespacesAndNewlines)
        if s.isEmpty { return nil }
        if !s.lowercased().hasPrefix("http://") && !s.lowercased().hasPrefix("https://") {
            s = "http://" + s
        }
        while s.hasSuffix("/") { s.removeLast() }
        return URL(string: s)
    }

    @MainActor
    private func test() async {
        guard let url = normalized() else {
            status = "Enter a valid URL."
            statusOK = false
            return
        }
        status = "Testing…"
        statusOK = false
        if await healthOK(url) {
            status = "Server is reachable."
            statusOK = true
        } else {
            status = "Cannot reach server at that URL."
            statusOK = false
        }
    }

    @MainActor
    private func connect() async {
        guard let url = normalized() else {
            status = "Enter a valid URL."
            statusOK = false
            return
        }
        if await healthOK(url) {
            onConnect(url)
        } else {
            status = "Server still unreachable. Check the address and that pinback-server is running."
            statusOK = false
        }
    }
}

private struct ShellView: View {
    let baseURL: URL
    let onOpenSettings: () -> Void

    @State private var page = WebPage()
    @State private var workspaces: [Workspace] = []
    @State private var activeId: String?
    @State private var selectedId: String?
    @State private var showInspector = false
    @State private var columnVisibility: NavigationSplitViewVisibility = .automatic

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
            WebView(page)
                .ignoresSafeArea()
                .backgroundExtensionEffect()
                .navigationTitle(active.map { $0.label.isEmpty ? $0.id : $0.label } ?? "Pinback")
                .toolbar {
                    ToolbarItem {
                        Button("Reload", systemImage: "arrow.clockwise") {
                            page.load(URLRequest(url: page.url ?? baseURL))
                        }
                    }
                    ToolbarSpacer(.flexible)
                    ToolbarItem {
                        Button("Server", systemImage: "network") {
                            onOpenSettings()
                        }
                    }
                    ToolbarItem {
                        if let shareURL = page.url {
                            ShareLink(item: shareURL)
                        }
                    }
                    ToolbarSpacer(.fixed)
                    ToolbarItem {
                        Button("Info", systemImage: "info.circle") {
                            showInspector.toggle()
                        }
                    }
                }
                .inspector(isPresented: $showInspector) {
                    InspectorView(active: active, count: workspaces.count)
                        .inspectorColumnWidth(min: 220, ideal: 280, max: 360)
                }
        }
        .task {
            page.load(URLRequest(url: baseURL))
            while !Task.isCancelled {
                await pollState()
                try? await Task.sleep(for: .seconds(1.5))
            }
        }
        .onChange(of: selectedId) { _, newValue in
            guard let id = newValue, id != activeId else { return }
            Task {
                _ = try? await page.callJavaScript(
                    "window.pinback && window.pinback.selectWorkspace(wid); return true;",
                    arguments: ["wid": id])
            }
        }
    }

    @MainActor
    private func pollState() async {
        let result: Any?
        do {
            result = try await page.callJavaScript(
                "return JSON.stringify(window.pinback ? window.pinback.state() : null);")
        } catch {
            return
        }
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
