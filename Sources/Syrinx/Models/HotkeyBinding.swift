import AppKit
import Foundation

/// A recorded global hotkey: a hardware virtual keycode (matches both
/// `NSEvent.keyCode` and Carbon's virtual keycodes) plus the modifier
/// flags held down. Resolves the spec's open question in favor of
/// requiring a non-modifier key + at least one modifier, since that's
/// what Carbon's `RegisterEventHotKey` reliably supports.
struct HotkeyBinding: Codable, Equatable, Hashable {
    var keyCode: UInt16
    var modifierFlags: UInt

    var displayString: String {
        let mods = NSEvent.ModifierFlags(rawValue: modifierFlags)
        var symbols = ""
        if mods.contains(.control) { symbols += "⌃" }
        if mods.contains(.option) { symbols += "⌥" }
        if mods.contains(.shift) { symbols += "⇧" }
        if mods.contains(.command) { symbols += "⌘" }
        return symbols + (Self.keyLabels[keyCode] ?? "Key\(keyCode)")
    }

    private static let keyLabels: [UInt16: String] = [
        0: "A", 1: "S", 2: "D", 3: "F", 4: "H", 5: "G", 6: "Z", 7: "X", 8: "C", 9: "V",
        11: "B", 12: "Q", 13: "W", 14: "E", 15: "R", 16: "Y", 17: "T",
        31: "O", 32: "U", 34: "I", 35: "P", 37: "L", 38: "J", 40: "K", 45: "N", 46: "M",
        18: "1", 19: "2", 20: "3", 21: "4", 23: "5", 22: "6", 26: "7", 28: "8", 25: "9", 29: "0",
        36: "Return", 48: "Tab", 49: "Space", 51: "Delete", 53: "Escape",
        123: "←", 124: "→", 125: "↓", 126: "↑",
        122: "F1", 120: "F2", 99: "F3", 118: "F4", 96: "F5", 97: "F6",
        98: "F7", 100: "F8", 101: "F9", 109: "F10", 103: "F11", 111: "F12"
    ]
}
