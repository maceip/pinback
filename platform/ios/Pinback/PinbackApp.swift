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
//
// Sugar (iOS / iPadOS 26 "Liquid Glass"):
//   - NavigationSplitView gives a native workspace sidebar + WebView detail that
//     adapts: two columns on iPad / landscape, a collapsing stack on iPhone (the
//     same structure also lights up nicely on foldables and Stage Manager).
//   - The toolbar is divided into sections with ToolbarSpacer(.flexible) and
//     ToolbarSpacer(.fixed); toolbar + sidebar adopt the system Liquid Glass
//     automatically, so we don't hand-roll glass (the only case that would need
//     a GlassEffectContainer).
//   - .backgroundExtensionEffect() lets the web content bleed edge-to-edge under
//     the chrome, and .inspector(isPresented:) shows active-workspace details.
//   - The cockpit's workspace list is mirrored into the native sidebar by polling
//     window.pinback.state() over WebPage.callJavaScript; selecting a row drives
//     window.pinback.selectWorkspace() back in the page (WebPage has no inbound
//     message channel, so this poll/call pair is the bridge).
@main
struct PinbackApp: App {
    var body: some Scene {
        WindowGroup {
            ShellView()
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

private struct ShellView: View {
    @State private var page = WebPage()
    @State private var workspaces: [Workspace] = []
    @State private var activeId: String?
    @State private var selectedId: String?
    @State private var showInspector = false
    @State private var columnVisibility: NavigationSplitViewVisibility = .automatic

    private var url: URL {
        URL(string: ProcessInfo.processInfo.environment["PINBACK_URL"]
            ?? "http://127.0.0.1:8088")!
    }

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
                            page.load(URLRequest(url: page.url ?? url))
                        }
                    }

                    ToolbarSpacer(.flexible)

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
            page.load(URLRequest(url: url))
            while !Task.isCancelled {
                await pollState()
                try? await Task.sleep(for: .seconds(1.5))
            }
        }
        .onChange(of: selectedId) { _, newValue in
            // User picked a workspace in the native sidebar -> drive the page.
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
        // Reflect the active workspace in the sidebar selection without looping
        // back through onChange (guarded by `id != activeId` there).
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
