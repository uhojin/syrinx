import AppKit
import HotKey

/// Registers/unregisters global Carbon hotkeys per pad via the HotKey
/// package, diffing against pads on every board change so unrelated
/// edits (rename, volume) don't churn the hotkey table.
final class HotkeyManager {
    var onTrigger: ((UUID) -> Void)?

    private var hotkeys: [UUID: HotKey] = [:]
    private var bindings: [UUID: HotkeyBinding] = [:]

    func sync(pads: [Pad]) {
        let padIDs = Set(pads.map(\.id))

        let staleIDs = bindings.keys.filter { !padIDs.contains($0) }
        for id in staleIDs {
            unregister(padID: id)
        }

        for pad in pads where bindings[pad.id] != pad.hotkey {
            if let binding = pad.hotkey {
                register(padID: pad.id, binding: binding)
            } else {
                unregister(padID: pad.id)
            }
        }
    }

    private func register(padID: UUID, binding: HotkeyBinding) {
        guard let key = Key(carbonKeyCode: UInt32(binding.keyCode)) else { return }
        let modifiers = NSEvent.ModifierFlags(rawValue: binding.modifierFlags)
        let hotkey = HotKey(key: key, modifiers: modifiers)
        hotkey.keyDownHandler = { [weak self] in self?.onTrigger?(padID) }
        hotkeys[padID] = hotkey
        bindings[padID] = binding
    }

    private func unregister(padID: UUID) {
        hotkeys.removeValue(forKey: padID)
        bindings.removeValue(forKey: padID)
    }
}
