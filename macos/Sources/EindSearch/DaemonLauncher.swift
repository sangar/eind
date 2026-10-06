import Foundation

/// Runs `eind serve` as a child of the app when no daemon answers the socket.
/// The owner stops it when the app quits; a launchd agent remains the way to
/// keep the daemon running all day.
@MainActor
final class DaemonLauncher {
    var onOutput: (String) -> Void = { _ in }
    var onExit: (Int32) -> Void = { _ in }

    private var process: Process?
    private var outputBuffer = Data()

    var isRunning: Bool { process?.isRunning == true }

    /// The agent `eind service enable` installs. When it exists the service
    /// owns the daemon and the app must not start a competing one.
    static var loginServiceInstalled: Bool {
        let agent = FileManager.default.homeDirectoryForCurrentUser
            .appending(path: "Library/LaunchAgents/eind.plist")
        return FileManager.default.fileExists(atPath: agent.path)
    }

    /// EIND_BINARY, then PATH, then the places `make` and Homebrew put binaries,
    /// because an app launched from Finder gets a minimal PATH.
    nonisolated static func locateBinary(
        environment: [String: String] = ProcessInfo.processInfo.environment,
        isExecutable: (String) -> Bool = { FileManager.default.isExecutableFile(atPath: $0) }
    ) -> String? {
        if let path = environment["EIND_BINARY"], !path.isEmpty {
            return path
        }
        let home = environment["HOME"] ?? NSHomeDirectory()
        var directories = (environment["PATH"] ?? "").split(separator: ":").map(String.init)
        directories += [home + "/.local/bin", "/opt/homebrew/bin", "/usr/local/bin"]
        return directories.map { $0 + "/eind" }.first(where: isExecutable)
    }

    func start(binary: String, socketPath: String) throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: binary)
        process.arguments = ["serve", "--socket", socketPath]
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        pipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            DispatchQueue.main.async {
                MainActor.assumeIsolated { self?.consume(data) }
            }
        }
        process.terminationHandler = { [weak self] process in
            let status = process.terminationStatus
            DispatchQueue.main.async {
                MainActor.assumeIsolated { self?.exited(status) }
            }
        }
        try process.run()
        self.process = process
    }

    func stop() {
        guard let process, process.isRunning else { return }
        process.terminate()
        process.waitUntilExit()
    }

    private func consume(_ data: Data) {
        outputBuffer.append(data)
        for line in drainLines(from: &outputBuffer) {
            onOutput(String(decoding: line, as: UTF8.self))
        }
    }

    private func exited(_ status: Int32) {
        (process?.standardError as? Pipe)?.fileHandleForReading.readabilityHandler = nil
        process = nil
        onExit(status)
    }
}
