import Foundation
import Network

/// A single connection to `eind serve`. The daemon cancels a request when the
/// next one arrives on the same connection, so sending a search per keystroke
/// is exactly what it expects. All callbacks are delivered on the main queue.
@MainActor
final class EindClient {
    enum Event {
        case connected
        case disconnected(String)
        case message(ServerMessage)
    }

    let socketPath: String
    var onEvent: (Event) -> Void = { _ in }

    private var connection: NWConnection?
    private var buffer = Data()
    private var nextID = 0

    init(socketPath: String = defaultSocketPath()) {
        self.socketPath = socketPath
    }

    func connect() {
        connection?.cancel()
        buffer.removeAll()
        let connection = NWConnection(to: .unix(path: socketPath), using: .tcp)
        self.connection = connection
        connection.stateUpdateHandler = { [weak self] state in
            MainActor.assumeIsolated {
                guard let self, self.connection === connection else { return }
                self.handle(state, of: connection)
            }
        }
        connection.start(queue: .main)
    }

    func disconnect() {
        connection?.cancel()
        connection = nil
    }

    /// Sends a search and returns its id so the caller can ignore stale replies.
    func search(_ query: String, limit: Int) -> Int {
        nextID += 1
        send(SearchRequest(id: nextID, query: query, limit: limit))
        return nextID
    }

    func requestStatus() {
        send(StatusRequest())
    }

    private func send(_ request: some Encodable) {
        guard let connection, var line = try? JSONEncoder().encode(request) else { return }
        line.append(UInt8(ascii: "\n"))
        connection.send(content: line, completion: .contentProcessed { _ in })
    }

    private func handle(_ state: NWConnection.State, of connection: NWConnection) {
        switch state {
        case .ready:
            onEvent(.connected)
            receive(on: connection)
        case .failed(let error), .waiting(let error):
            onEvent(.disconnected(error.localizedDescription))
        case .cancelled:
            onEvent(.disconnected("connection closed"))
        default:
            break
        }
    }

    private func receive(on connection: NWConnection) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 1 << 20) { [weak self] data, _, isComplete, error in
            MainActor.assumeIsolated {
                guard let self, self.connection === connection else { return }
                self.handleReceived(data, isComplete: isComplete, error: error, on: connection)
            }
        }
    }

    private func handleReceived(_ data: Data?, isComplete: Bool, error: NWError?, on connection: NWConnection) {
        if let data {
            buffer.append(data)
            for line in drainLines(from: &buffer) {
                if let message = try? JSONDecoder().decode(ServerMessage.self, from: line) {
                    onEvent(.message(message))
                }
            }
        }
        if let error {
            onEvent(.disconnected(error.localizedDescription))
        } else if isComplete {
            onEvent(.disconnected("daemon closed the connection"))
        } else {
            receive(on: connection)
        }
    }
}
