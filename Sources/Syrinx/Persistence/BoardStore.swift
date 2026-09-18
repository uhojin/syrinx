import Foundation

/// Reads/writes the board to
/// ~/Library/Application Support/Syrinx/board.json.
/// Unsandboxed build, so plain absolute file paths are enough — no
/// security-scoped bookmarks needed (see spec's open questions).
enum BoardStore {
    private static let appFolderName = "Syrinx"
    private static let fileName = "board.json"

    private static var fileURL: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let dir = base.appendingPathComponent(appFolderName, isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent(fileName)
    }

    static func load() -> [Pad] {
        guard
            let data = try? Data(contentsOf: fileURL),
            let pads = try? JSONDecoder().decode([Pad].self, from: data),
            !pads.isEmpty
        else {
            return Board.emptyGrid(count: 9)
        }
        return pads
    }

    static func save(_ pads: [Pad]) {
        guard let data = try? JSONEncoder().encode(pads) else { return }
        try? data.write(to: fileURL, options: .atomic)
    }
}
