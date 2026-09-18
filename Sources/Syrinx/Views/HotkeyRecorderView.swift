import AppKit
import SwiftUI

/// Native SwiftUI control for recording a global hotkey. Click, then
/// press a modifier + key combo. Escape cancels, Delete clears.
/// Requires a non-modifier key plus at least one modifier — bare
/// modifier presses are ignored, matching what Carbon's
/// RegisterEventHotKey can reliably register.
///
/// SwiftUI's `onKeyPress` reports layout-translated characters, not the
/// hardware virtual keycodes Carbon's global hotkey API needs, so
/// capture uses a local `NSEvent` monitor scoped strictly to the
/// recording window. The monitor is torn down explicitly on capture,
/// focus loss, and disappear, *and* as a hard safety net via a
/// class's `deinit` — relying only on `.onDisappear`/`.onChange`
/// firing left a real bug: if a dismiss path skipped them, the
/// monitor leaked and silently swallowed every keydown app-wide
/// (looking exactly like "text fields stopped accepting input").
struct HotkeyRecorderView: View {
    @Binding var binding: HotkeyBinding?

    @State private var isRecording = false
    @FocusState private var isFocused: Bool
    @State private var monitorBox = KeyMonitorBox()

    var body: some View {
        Button {
            isRecording = true
            isFocused = true
        } label: {
            Text(labelText)
                .frame(minWidth: 140)
        }
        .buttonStyle(.bordered)
        .tint(isRecording ? Color.accentColor : nil)
        .focused($isFocused)
        .help("Click, then press a modifier + key combo to assign a global shortcut.")
        .accessibilityLabel(binding == nil ? "No hotkey assigned" : "Hotkey \(binding!.displayString)")
        .onChange(of: isFocused) { _, focused in
            if !focused { stopRecording() }
        }
        .onChange(of: isRecording) { _, recording in
            if recording {
                startMonitor()
            } else {
                stopMonitor()
            }
        }
        .onDisappear { stopMonitor() }
    }

    private var labelText: String {
        if isRecording { return "Press keys… (Esc cancels)" }
        return binding?.displayString ?? "Record Hotkey…"
    }

    private func startMonitor() {
        stopMonitor()
        monitorBox.monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { event in
            handle(event)
            return nil // swallow while recording so it doesn't also act as a normal keypress
        }
    }

    private func stopMonitor() {
        monitorBox.removeIfNeeded()
    }

    private func stopRecording() {
        isRecording = false
        stopMonitor()
    }

    private func handle(_ event: NSEvent) {
        defer { stopRecording() }

        if event.keyCode == 53 { return } // Escape cancels
        if event.keyCode == 51 { // Delete/backspace clears
            binding = nil
            return
        }

        let modifiers = event.modifierFlags.intersection([.command, .option, .control, .shift])
        guard !modifiers.isEmpty else { return }
        binding = HotkeyBinding(keyCode: event.keyCode, modifierFlags: modifiers.rawValue)
    }
}

/// Holds the raw NSEvent monitor token and guarantees removal in
/// `deinit` — a hard safety net independent of whichever SwiftUI
/// lifecycle callback does or doesn't fire on a given dismiss path.
private final class KeyMonitorBox {
    var monitor: Any?

    func removeIfNeeded() {
        guard let monitor else { return }
        NSEvent.removeMonitor(monitor)
        self.monitor = nil
    }

    deinit {
        if let monitor {
            NSEvent.removeMonitor(monitor)
        }
    }
}
