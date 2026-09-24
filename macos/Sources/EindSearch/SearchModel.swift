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

    var isConnected: Bool { connectionError == nil }

    private let client: EindClient
    private let resultLimit = 200
    private var latestRequestID = 0
    private var reconnect: Task<Void, Never>?

    init(client: EindClient = EindClient()) {
        self.client = client
        connectionError = "connecting to \(client.socketPath)"
        client.onEvent = { [weak self] event in self?.handle(event) }
        client.connect()
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
        NSWorkspace.shared.open(URL(fileURLWithPath: result.path))
    }

    func revealInFinder(_ result: SearchResult) {
        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: result.path)])
    }

    private func handle(_ event: EindClient.Event) {
        switch event {
        case .connected:
            connectionError = nil
            client.requestStatus()
            search()
        case .disconnected(let reason):
            connectionError = "eind daemon not reachable at \(client.socketPath) (\(reason)). Start it with `eind serve`."
            scheduleReconnect()
        case .message(let message):
            apply(message)
        }
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
