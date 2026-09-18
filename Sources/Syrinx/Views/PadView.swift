import SwiftUI
import UniformTypeIdentifiers

/// A single pad: native bordered button chrome. Playing uses
/// `.borderedProminent` for a native "pressed in" active look, per
/// feedback that a plain border overlay didn't read as "pressed."
///
/// Reads playback state directly from the environment (rather than
/// taking it as a parameter from BoardView) so only this pad
/// re-renders when playback changes — not the whole board. BoardView
/// used to read `audioEngine.playingPadIDs` itself, which made its
/// *entire* body (including the edit-sheet presentation) re-evaluate
/// on every playback tick, which was kicking keyboard focus out of
/// the name TextField on every keystroke.
@MainActor
struct PadView: View {
    @Environment(AudioEngine.self) private var audioEngine

    let pad: Pad
    var onTap: () -> Void
    var onDropFile: (URL) -> Void
    var onEdit: () -> Void

    @State private var isTargeted = false
    @State private var isPlayingSnapshot = false

    // `isPlaying` is read fresh below, but SwiftUI's fine-grained
    // `@Observable` tracking has a real, reproducible gap around
    // views covered/uncovered by a sheet: a pad's highlight fails to
    // refresh specifically after visiting Call Mode (which reads many
    // `AudioEngine` properties itself) even though `playingPadIDs`
    // never actually changed — while the pad editor sheet (which
    // doesn't touch `AudioEngine` at all) doesn't show the gap.
    // Consolidating BoardView's sheets into one didn't close it, so
    // `isPlayingSnapshot` explicitly resyncs on every appearance and
    // on every `playingPadIDs` change, independent of body
    // re-evaluation timing.
    private var isPlaying: Bool {
        audioEngine.playingPadIDs.contains(pad.id)
    }

    var body: some View {
        Group {
            if isPlayingSnapshot {
                buttonLabel.buttonStyle(.borderedProminent)
            } else {
                buttonLabel.buttonStyle(.bordered)
            }
        }
        .controlSize(.large)
        .overlay(
            RoundedRectangle(cornerRadius: 8)
                .stroke(isTargeted ? Color.accentColor : .clear, lineWidth: 2)
        )
        .help(pad.isEmpty ? "Click to add a clip, or drag one here" : pad.name)
        .accessibilityLabel(pad.isEmpty ? "Empty pad" : "Play \(pad.name)")
        .contextMenu {
            Button("Edit…", action: onEdit)
        }
        .onDrop(of: [.fileURL], isTargeted: $isTargeted) { providers in
            guard let provider = providers.first else { return false }
            _ = provider.loadObject(ofClass: URL.self) { url, _ in
                guard let url else { return }
                DispatchQueue.main.async { onDropFile(url) }
            }
            return true
        }
        .onAppear { isPlayingSnapshot = isPlaying }
        .onChange(of: audioEngine.playingPadIDs) { _, newValue in
            isPlayingSnapshot = newValue.contains(pad.id)
        }
    }

    private var buttonLabel: some View {
        Button(action: onTap) {
            VStack(spacing: 6) {
                Image(systemName: pad.isEmpty ? "plus" : "waveform")
                    .font(.title2)
                Text(pad.name)
                    .font(.callout)
                    .lineLimit(1)
                    .truncationMode(.middle)
                if let hotkey = pad.hotkey {
                    Text(hotkey.displayString)
                        .font(.caption)
                }
            }
            .frame(maxWidth: .infinity, minHeight: 76)
            .padding(8)
        }
    }
}
