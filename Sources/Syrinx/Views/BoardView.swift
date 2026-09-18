import SwiftUI
import UniformTypeIdentifiers

/// Grid of pads. Tapping an empty pad opens a file picker; tapping a
/// filled pad plays it. Drag-and-drop onto any pad assigns/replaces
/// its clip.
@MainActor
struct BoardView: View {
    @Environment(AudioEngine.self) private var audioEngine
    @Environment(Board.self) private var board

    private enum Sheet: Identifiable {
        case callMode
        case editPad(Pad)

        var id: String {
            switch self {
            case .callMode: return "callMode"
            case .editPad(let pad): return pad.id.uuidString
            }
        }
    }

    @State private var activeSheet: Sheet?
    @State private var pendingImportPadID: UUID?
    @State private var isImporterPresented = false

    private let columns = [GridItem(.adaptive(minimum: 110, maximum: 160), spacing: 12)]

    var body: some View {
        Group {
            if board.pads.isEmpty {
                ContentUnavailableView {
                    Label("No Pads", systemImage: "square.grid.3x3")
                } description: {
                    Text("Add a pad to start building your soundboard.")
                } actions: {
                    Button("Add Pad") { board.addEmptyPad() }
                }
            } else {
                ScrollView {
                    LazyVGrid(columns: columns, spacing: 12) {
                        ForEach(board.pads) { pad in
                            PadView(
                                pad: pad,
                                onTap: { handleTap(pad) },
                                onDropFile: { url in handleDrop(url: url, padID: pad.id) },
                                onEdit: { activeSheet = .editPad(pad) }
                            )
                        }
                    }
                    .padding(16)
                }
            }
        }
        .toolbar {
            ToolbarItem {
                Button {
                    activeSheet = .callMode
                } label: {
                    Label("Call Mode", systemImage: audioEngine.isCallModeEnabled ? "phone.fill" : "phone")
                }
                .foregroundStyle(audioEngine.isCallModeEnabled ? Color.green : Color.primary)
                .help("Set up routing pad audio into a voice call")
            }
            ToolbarItem {
                Button {
                    board.addEmptyPad()
                } label: {
                    Label("Add Pad", systemImage: "plus.square")
                }
            }
        }
        .sheet(item: $activeSheet) { sheet in
            switch sheet {
            case .callMode:
                CallModeView()
            case .editPad(let pad):
                PadEditorView(pad: pad, onSave: { updated in
                    board.update(updated)
                }, onDelete: {
                    board.remove(padID: pad.id)
                })
            }
        }
        .fileImporter(
            isPresented: $isImporterPresented,
            allowedContentTypes: [.mp3, .wav, .audio],
            allowsMultipleSelection: false
        ) { result in
            guard case .success(let urls) = result, let url = urls.first, let padID = pendingImportPadID else { return }
            handleDrop(url: url, padID: padID)
            pendingImportPadID = nil
        }
    }

    private func handleTap(_ pad: Pad) {
        if pad.isEmpty {
            pendingImportPadID = pad.id
            isImporterPresented = true
        } else {
            audioEngine.play(pad: pad)
        }
    }

    private func handleDrop(url: URL, padID: UUID) {
        guard var pad = board.pads.first(where: { $0.id == padID }) else { return }
        pad.filePath = url.path
        if pad.name.isEmpty || pad.name == Pad.defaultName {
            pad.name = url.deletingPathExtension().lastPathComponent
        }
        board.update(pad)
    }

}
