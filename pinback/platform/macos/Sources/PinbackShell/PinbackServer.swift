import Foundation

// Launches and supervises the bundled `pinback-server` for the desktop shell.
//
// The shell defers ALL platform/webview decisions to the parent project; this
// type encodes the one app-level decision the shell owns: a desktop Pinback is
// self-contained, so it runs its own loopback server unless told otherwise.
@MainActor
final class PinbackServer {
    // Fixed loopback endpoint. Loopback-only: never reachable off-machine.
    static let host = "127.0.0.1"
    static let port = 8088

    private var process: Process?

    // Returns the URL the webview should load. If PINBACK_URL is set we honor
    // it and never spawn (dev/remote mode). Otherwise we spawn the bundled
    // server, wait until /healthz answers, and return its loopback URL.
    func start() async -> URL {
        let env = ProcessInfo.processInfo.environment
        if let override = env["PINBACK_URL"], let u = URL(string: override) {
            return u
        }

        let base = URL(string: "http://\(Self.host):\(Self.port)")!
        guard let bin = Self.serverBinary() else {
            // No bundled/PATH server found: load the base URL anyway so the
            // webview shows a connection error instead of a blank window.
            return base
        }

        let proc = Process()
        proc.executableURL = bin
        var args = ["--bind", "\(Self.host):\(Self.port)", "--quiet"]
        // Optional pass-through so a packaged app can point at a specific model
        // / agent binary without rebuilding the shell.
        if let model = env["PINBACK_MODEL"], !model.isEmpty {
            args += ["--model", model]
        }
        if let agent = env["PINBACK_AGENT_BIN"], !agent.isEmpty {
            args += ["--agent-bin", agent]
        }
        proc.arguments = args
        // Inherit env so ds4-agent picks up the user's Metal / model setup.
        do {
            try proc.run()
            process = proc
        } catch {
            return base
        }

        await Self.waitForHealth(base: base)
        return base
    }

    func stop() {
        process?.terminate()
        process = nil
    }

    // Locate pinback-server: prefer one bundled next to the app executable
    // (Contents/MacOS/pinback-server), then Resources, then a Homebrew/PATH
    // install for `swift run` iteration.
    private static func serverBinary() -> URL? {
        let fm = FileManager.default
        var candidates: [URL] = []
        if let exeDir = Bundle.main.executableURL?.deletingLastPathComponent() {
            candidates.append(exeDir.appendingPathComponent("pinback-server"))
        }
        if let res = Bundle.main.resourceURL {
            candidates.append(res.appendingPathComponent("pinback-server"))
        }
        for p in ["/opt/homebrew/bin/pinback-server",
                  "/usr/local/bin/pinback-server"] {
            candidates.append(URL(fileURLWithPath: p))
        }
        return candidates.first { fm.isExecutableFile(atPath: $0.path) }
    }

    // Poll /healthz until it returns 200 (or we time out and load anyway).
    private static func waitForHealth(base: URL) async {
        let health = base.appendingPathComponent("healthz")
        let deadline = Date().addingTimeInterval(30)
        var req = URLRequest(url: health)
        req.timeoutInterval = 1
        while Date() < deadline {
            if let (_, resp) = try? await URLSession.shared.data(for: req),
               let http = resp as? HTTPURLResponse, http.statusCode == 200 {
                return
            }
            try? await Task.sleep(nanoseconds: 200_000_000) // 200ms
        }
    }
}
