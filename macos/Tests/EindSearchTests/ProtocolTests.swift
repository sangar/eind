import Foundation
import Testing
@testable import EindSearch

@Suite struct ProtocolTests {
    @Test func decodesSearchResponse() throws {
        let line = Data("""
        {"id":7,"total":132,"elapsed_ms":2.1,"results":[{"path":"/home/me/x/report.pdf","name":"report.pdf","type":"file","size":1024,"modified":"2024-03-13T10:00:00+01:00","created":"2024-03-01T08:00:00+01:00"}]}
        """.utf8)
        let message = try JSONDecoder().decode(ServerMessage.self, from: line)
        #expect(message.id == 7)
        #expect(message.total == 132)
        #expect(message.results?.first?.name == "report.pdf")
        #expect(message.results?.first?.parentPath == "/home/me/x")
        #expect(message.results?.first?.modifiedDate != nil)
        #expect(message.status == nil)
    }

    @Test func decodesStatusErrorAndCancellation() throws {
        let status = try JSONDecoder().decode(ServerMessage.self, from: Data(#"{"files":10,"folders":2,"roots":["/home/me"],"built":"2024-03-13T09:00:00+01:00","index":"/x/index.bin"}"#.utf8))
        #expect(status.status?.files == 10)
        #expect(status.status?.roots == ["/home/me"])

        let error = try JSONDecoder().decode(ServerMessage.self, from: Data(#"{"id":1,"error":"size: bad"}"#.utf8))
        #expect(error.error == "size: bad")

        let cancelled = try JSONDecoder().decode(ServerMessage.self, from: Data(#"{"id":1,"cancelled":true}"#.utf8))
        #expect(cancelled.cancelled == true)
        #expect(cancelled.results == nil)
    }

    @Test func drainsCompleteLinesAndKeepsPartialTail() {
        var buffer = Data("{\"a\":1}\n\n{\"b\":2}\n{\"c\"".utf8)
        let lines = drainLines(from: &buffer)
        #expect(lines.map { String(decoding: $0, as: UTF8.self) } == ["{\"a\":1}", "{\"b\":2}"])
        #expect(String(decoding: buffer, as: UTF8.self) == "{\"c\"")
    }

    @Test func socketPathFollowsDaemonDefaults() {
        #expect(defaultSocketPath(environment: ["EIND_SOCKET": "/s.sock", "XDG_RUNTIME_DIR": "/run/user/1"]) == "/s.sock")
        #expect(defaultSocketPath(environment: ["XDG_RUNTIME_DIR": "/run/user/1"]) == "/run/user/1/eind.sock")
        #expect(defaultSocketPath(environment: ["TMPDIR": "/var/t/"]) == "/var/t/eind-\(getuid()).sock")
        #expect(defaultSocketPath(environment: [:]) == "/tmp/eind-\(getuid()).sock")
    }
}

@Suite struct DaemonLauncherTests {
    @Test func locatesBinaryFromOverridePathAndKnownDirectories() {
        let existing: Set<String> = ["/opt/homebrew/bin/eind", "/Users/me/go/bin/eind"]
        let isExecutable = { existing.contains($0) }

        #expect(DaemonLauncher.locateBinary(environment: ["EIND_BINARY": "/x/eind"], isExecutable: isExecutable) == "/x/eind")
        #expect(DaemonLauncher.locateBinary(environment: ["PATH": "/usr/bin:/opt/homebrew/bin", "HOME": "/Users/me"], isExecutable: isExecutable) == "/opt/homebrew/bin/eind")
        #expect(DaemonLauncher.locateBinary(environment: ["PATH": "/usr/bin", "HOME": "/Users/me"], isExecutable: isExecutable) == "/Users/me/go/bin/eind")
        #expect(DaemonLauncher.locateBinary(environment: ["PATH": "/usr/bin", "HOME": "/Users/me"], isExecutable: { _ in false }) == nil)
    }
}
