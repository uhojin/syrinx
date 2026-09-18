import Foundation

/// A single pad on the board: the clip it plays, how loud, and the
/// global hotkey that triggers it.
/// Array order in `Board.pads` doubles as grid position.
struct Pad: Identifiable, Codable, Equatable {
    static let defaultName = "Empty"

    var id: UUID
    var name: String
    var filePath: String?
    var volume: Float
    var hotkey: HotkeyBinding?

    init(
        id: UUID = UUID(),
        name: String = Pad.defaultName,
        filePath: String? = nil,
        volume: Float = 1.0,
        hotkey: HotkeyBinding? = nil
    ) {
        self.id = id
        self.name = name
        self.filePath = filePath
        self.volume = volume
        self.hotkey = hotkey
    }

    var isEmpty: Bool { filePath == nil }

    var fileURL: URL? {
        filePath.map { URL(fileURLWithPath: $0) }
    }
}
