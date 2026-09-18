import Foundation

/// Wires Board <-> AudioEngine <-> HotkeyManager together and owns
/// debounced autosave to disk. Handed to views via `.environment`
/// (board/audioEngine individually, not this wrapper itself).
@MainActor
final class AppEnvironment: ObservableObject {
    let board: Board
    let audioEngine: AudioEngine

    private let hotkeyManager = HotkeyManager()
    private var saveWorkItem: DispatchWorkItem?

    init() {
        let board = Board(pads: BoardStore.load())
        let audioEngine = AudioEngine()
        self.board = board
        self.audioEngine = audioEngine

        hotkeyManager.onTrigger = { [weak board, weak audioEngine] padID in
            // HotKey's callback isn't guaranteed to land on the main
            // actor (it crosses a Carbon/C callback boundary the
            // compiler can't verify) — hop explicitly before touching
            // `audioEngine`, which is main-actor-isolated.
            Task { @MainActor in
                guard let pad = board?.pads.first(where: { $0.id == padID }) else { return }
                audioEngine?.play(pad: pad)
            }
        }

        hotkeyManager.sync(pads: board.pads)
        audioEngine.restoreCallModeIfNeeded()

        board.onPadsChanged = { [weak self] pads in
            self?.hotkeyManager.sync(pads: pads)
            self?.scheduleSave(pads: pads)
        }
    }

    private func scheduleSave(pads: [Pad]) {
        saveWorkItem?.cancel()
        let item = DispatchWorkItem { BoardStore.save(pads) }
        saveWorkItem = item
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5, execute: item)
    }
}
