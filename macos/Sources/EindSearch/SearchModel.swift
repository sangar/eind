import AppKit
import Foundation
import Observation

@MainActor
@Observable
final class SearchModel {
    var query = ""
    var results: [SearchResult] = []
    var total = 0
    var elapsedMs = 0.0
    var queryError: String?
    var indexStatus: IndexStatus?
    var connectionError: String?
    var daemonOutput: String?

    var isConnected: Bool { connectionError == nil }

    private let client: EindClient
    private let launcher: DaemonLauncher
    private let resultLimit = 200
    private var latestRequestID = 0
    private var reconnect: Task<Void, Never>?
    private var daemonLaunchAttempted = false

    init(client: EindClient = EindClient(), launcher: DaemonLauncher = DaemonLauncher()) {
        self.client = client
        self.launcher = launcher
        connectionError = "Connecting to \(client.socketPath)"
        client.onEvent = { [weak self] event in self?.handle(event) }
        launcher.onOutput = { [weak self] line in self?.daemonOutput = line }
        launcher.onExit = { [weak self] status in self?.daemonExited(status) }
        NotificationCenter.default.addObserver(forName: NSApplication.willTerminateNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.shutDown() }
        }
        client.connect()
    }

    /// Drops our connection before stopping the daemon we started, because the
    /// daemon's shutdown waits for clients to go away.
    private func shutDown() {
        reconnect?.cancel()
        client.disconnect()
        launcher.stop()
    }

    func search() {
        queryError = nil
        guard !query.trimmingCharacters(in: .whitespaces).isEmpty else {
            results = []
            total = 0
            latestRequestID += 1
            return
        }
        latestRequestID = client.search(query, limit: resultLimit)
    }

    func open(_ result: SearchResult) {
        NSWorkspace.shared.open(result.url)
    }

    func revealInFinder(_ result: SearchResult) {
        NSWorkspace.shared.activateFileViewerSelecting([result.url])
    }

    private func handle(_ event: EindClient.Event) {
        switch event {
        case .connected:
            connectionError = nil
            daemonOutput = nil
            daemonLaunchAttempted = false
            client.requestStatus()
            search()
        case .disconnected(let reason):
            if !launcher.isRunning {
                connectionError = "No eind daemon at \(client.socketPath) (\(reason))."
                startDaemonIfNeeded()
            }
            scheduleReconnect()
        case .message(let message):
            apply(message)
        }
    }

    private func startDaemonIfNeeded() {
        guard !daemonLaunchAttempted else { return }
        daemonLaunchAttempted = true
        if DaemonLauncher.loginServiceInstalled {
            connectionError = "The eind login service is enabled but not answering yet. Waiting for it."
            return
        }
        guard let binary = DaemonLauncher.locateBinary() else {
            connectionError! += " The eind binary was not found; install it with `go install`, or set EIND_BINARY."
            return
        }
        do {
            try launcher.start(binary: binary, socketPath: client.socketPath)
            connectionError = "Starting \(binary) serve. On first run this builds the index, which can take a few minutes."
        } catch {
            connectionError! += " Starting \(binary) failed: \(error.localizedDescription)"
        }
    }

    private func daemonExited(_ status: Int32) {
        connectionError = "eind serve exited with status \(status)."
    }

    private func apply(_ message: ServerMessage) {
        if let status = message.status {
            indexStatus = status
            return
        }
        guard message.id == latestRequestID else { return }
        if let error = message.error {
            queryError = error
        } else if let results = message.results {
            self.results = results
            total = message.total ?? results.count
            elapsedMs = message.elapsed_ms ?? 0
        }
    }

    private func scheduleReconnect() {
        reconnect?.cancel()
        reconnect = Task { [weak self] in
            try? await Task.sleep(for: .seconds(2))
            guard !Task.isCancelled else { return }
            self?.client.connect()
        }
    }
}
