import Foundation
import Observation

/// The full collection of pads. Array order is grid position.
///
/// `@Observable` (not `ObservableObject`/`@Published`) so views only
/// re-render when a property they actually read changes, instead of
/// on every mutation anywhere in the object.
@Observable
final class Board {
    var pads: [Pad]

    /// Called after any mutation, with the new pads array. Used by
    /// AppEnvironment to re-sync hotkeys and debounce-save — a plain
    /// callback since `@Observable` doesn't provide Combine `$`
    /// publishers the way `@Published` did.
    var onPadsChanged: (([Pad]) -> Void)?

    init(pads: [Pad] = Board.emptyGrid(count: 9)) {
        self.pads = pads
    }

    static func emptyGrid(count: Int) -> [Pad] {
        (0..<count).map { _ in Pad() }
    }

    func addEmptyPad() {
        pads.append(Pad())
        onPadsChanged?(pads)
    }

    func update(_ pad: Pad) {
        guard let index = pads.firstIndex(where: { $0.id == pad.id }) else { return }
        pads[index] = pad
        onPadsChanged?(pads)
    }

    func remove(padID: UUID) {
        pads.removeAll { $0.id == padID }
        onPadsChanged?(pads)
    }
}
