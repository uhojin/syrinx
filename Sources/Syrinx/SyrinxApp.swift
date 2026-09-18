import AppKit
import SwiftUI

/// Xcode's Run button launches an SPM `executableTarget` as a bare
/// Mach-O binary (`.../Debug/Syrinx`), not through a real `.app` bundle
/// via LaunchServices (that would be `.../Debug/Syrinx.app/Contents/
/// MacOS/Syrinx`). Apps launched that way can render and take mouse
/// input, but never get properly activated for keyboard routing — no
/// text input works, and not even `Cmd+Q` fires, since menu key
/// equivalents and first-responder key events both require the app to
/// actually be the active app, not just frontmost-looking. Explicitly
/// activating fixes it under that launch path, and is a no-op under a
/// normal bundled launch (`open Syrinx.app` / double-click in Finder),
/// where activation already happens correctly.
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }
}

@main
struct SyrinxApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var appEnvironment = AppEnvironment()

    var body: some Scene {
        // A single, non-duplicable window — Syrinx has exactly one
        // board, so `Window` (not `WindowGroup`) drops the "New Window"
        // menu command that would otherwise spawn a second window onto
        // the same singleton board/audio engine.
        Window("Syrinx", id: "main") {
            BoardView()
                .environment(appEnvironment.board)
                .environment(appEnvironment.audioEngine)
                .frame(minWidth: 480, minHeight: 344)
        }
        .defaultSize(width: 480, height: 344)
    }
}
