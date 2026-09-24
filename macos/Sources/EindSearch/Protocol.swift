import Foundation

struct SearchRequest: Encodable {
    var id: Int
    var query: String
    var limit: Int
}

struct StatusRequest: Encodable {
    var op = "status"
}

struct SearchResult: Decodable, Identifiable, Hashable {
    var path: String
    var name: String
    var type: String
    var size: Int64
    var modified: String

    var id: String { path }
    var isDirectory: Bool { type == "dir" }
    var parentPath: String { (path as NSString).deletingLastPathComponent }
    var modifiedDate: Date? { ISO8601DateFormatter().date(from: modified) }
}

struct IndexStatus: Decodable {
    var files: Int
    var folders: Int
    var roots: [String]
}

/// One JSON line from the daemon. Which fields are present tells the kind of
/// message: a search response, a cancellation, an error or a status.
struct ServerMessage: Decodable {
    var id: Int?
    var total: Int?
    var elapsed_ms: Double?
    var results: [SearchResult]?
    var cancelled: Bool?
    var error: String?
    var files: Int?
    var folders: Int?
    var roots: [String]?

    var status: IndexStatus? {
        guard let files, let folders else { return nil }
        return IndexStatus(files: files, folders: folders, roots: roots ?? [])
    }
}

/// Mirrors the daemon's default: EIND_SOCKET, then $XDG_RUNTIME_DIR/eind.sock,
/// then eind-<uid>.sock in the temp directory.
func defaultSocketPath(environment: [String: String] = ProcessInfo.processInfo.environment) -> String {
    if let path = environment["EIND_SOCKET"], !path.isEmpty {
        return path
    }
    if let runtimeDir = environment["XDG_RUNTIME_DIR"], !runtimeDir.isEmpty {
        return runtimeDir + "/eind.sock"
    }
    let tempDir = environment["TMPDIR"].flatMap { $0.isEmpty ? nil : $0 } ?? "/tmp"
    return (tempDir as NSString).appendingPathComponent("eind-\(getuid()).sock")
}

/// Splits a receive buffer into complete JSON lines, leaving any partial
/// trailing line in the buffer.
func drainLines(from buffer: inout Data) -> [Data] {
    var lines: [Data] = []
    while let newline = buffer.firstIndex(of: UInt8(ascii: "\n")) {
        let line = buffer.subdata(in: buffer.startIndex..<newline)
        buffer.removeSubrange(buffer.startIndex...newline)
        if !line.isEmpty {
            lines.append(line)
        }
    }
    return lines
}
