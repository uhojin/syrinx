import SwiftUI
import UniformTypeIdentifiers

/// Sheet: rename, choose/clear clip, set volume. Global hotkey
/// assignment (`Pad.hotkey`, `HotkeyManager`, `HotkeyRecorderView`)
/// is implemented but its UI section is commented out below pending
/// more testing — the model/manager stay wired so existing
/// assignments keep working.
///
/// Grouped form styling matches native macOS settings-style sheets
/// (and `CallModeView`).
struct PadEditorView: View {
    @State private var draft: Pad
    var onSave: (Pad) -> Void
    var onDelete: () -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var isImporterPresented = false
    @State private var isDeleteConfirmationPresented = false
    @FocusState private var isNameFieldFocused: Bool

    init(pad: Pad, onSave: @escaping (Pad) -> Void, onDelete: @escaping () -> Void) {
        self._draft = State(initialValue: pad)
        self.onSave = onSave
        self.onDelete = onDelete
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("Edit Pad")
                .font(.headline)
                .padding([.top, .horizontal])

            Form {
                Section("General") {
                    TextField("Name", text: $draft.name)
                        .focused($isNameFieldFocused)
                }

                Section("Playback") {
                    LabeledContent("Clip") {
                        HStack {
                            Text(draft.fileURL?.lastPathComponent ?? "None")
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                            Spacer()
                            Button("Choose…") { isImporterPresented = true }
                        }
                    }
                    LabeledContent("Volume") {
                        Slider(value: Binding(
                            get: { Double(draft.volume) },
                            set: { draft.volume = Float($0) }
                        ), in: 0...1)
                    }
                }
                // Disabled for now — re-enable by uncommenting once
                // hotkey recording has had more real-world testing.
                // Section {
                //     LabeledContent("Global Shortcut") {
                //         HotkeyRecorderView(binding: $draft.hotkey)
                //     }
                // } footer: {
                //     Text("Click, then press a modifier + key combo. Works even when Syrinx isn't the active app.")
                //         .font(.footnote)
                //         .foregroundStyle(.secondary)
                // }
            }
            .formStyle(.grouped)
            .fixedSize(horizontal: false, vertical: true)

            HStack {
                Button("Delete Pad…", role: .destructive) {
                    isDeleteConfirmationPresented = true
                }
                .buttonStyle(.borderedProminent)
                .tint(.red)
                if !draft.isEmpty {
                    Button("Clear Clip", role: .destructive) {
                        draft.filePath = nil
                        draft.name = Pad.defaultName
                    }
                }
                Spacer()
                Button("Cancel") { dismiss() }
                Button("Save") {
                    if draft.name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                        draft.name = Pad.defaultName
                    }
                    onSave(draft)
                    dismiss()
                }
                .keyboardShortcut(.defaultAction)
            }
            .padding()
        }
        .frame(width: 380)
        .onAppear {
            DispatchQueue.main.async {
                isNameFieldFocused = true
            }
        }
        .fileImporter(
            isPresented: $isImporterPresented,
            allowedContentTypes: [.mp3, .wav, .audio],
            allowsMultipleSelection: false
        ) { result in
            guard case .success(let urls) = result, let url = urls.first else { return }
            draft.filePath = url.path
            if draft.name.isEmpty || draft.name == Pad.defaultName {
                draft.name = url.deletingPathExtension().lastPathComponent
            }
        }
        .confirmationDialog(
            "Delete \"\(draft.name)\"?",
            isPresented: $isDeleteConfirmationPresented,
            titleVisibility: .visible
        ) {
            Button("Delete Pad", role: .destructive) {
                onDelete()
                dismiss()
            }
        } message: {
            Text("This removes the pad from the board, including its position and any hotkey. This can't be undone.")
        }
    }
}
