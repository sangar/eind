import AppKit
import SwiftUI

@main
struct EindSearchApp: App {
    var body: some Scene {
        WindowGroup("Eind Search") {
            ContentView()
                .onAppear(perform: bringToFront)
        }
    }

    /// `swift run` starts the process without an app bundle, so it would
    /// otherwise open its window behind the terminal without a Dock icon.
    private func bringToFront() {
        NSApplication.shared.setActivationPolicy(.regular)
        NSApplication.shared.activate(ignoringOtherApps: true)
    }
}
