import CoreAudio
import SwiftUI

/// Sheet for setting up "Call Mode": install/uninstall the Syrinx
/// Loopback driver, pick which mic to use, and toggle routing pad
/// audio + your live mic (mixed together) into it. Once on, pick
/// "Syrinx Microphone" directly as your input in Discord/Zoom/etc. —
/// it carries both your voice and pad audio, so no separate
/// combined-mic device is needed.
@MainActor
struct CallModeView: View {
    @Environment(AudioEngine.self) private var audioEngine
    @Environment(\.dismiss) private var dismiss

    @State private var isDriverInstalled = LoopbackDriver.isInstalled
    @State private var isBusy = false
    @State private var actionError: String?

    @State private var availableMics: [(uid: String, name: String, deviceID: AudioDeviceID)] = []
    @State private var selectedMicUID: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("Call Mode")
                    .font(.title3.weight(.semibold))
                Spacer()
                Button("Done") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
            Text("Mix your mic with pad audio for voice calls.")
                .font(.subheadline)
                .foregroundStyle(.secondary)

            Form {
                Section {
                    LabeledContent("Loopback Driver") {
                        HStack(spacing: 6) {
                            statusIcon(isDriverInstalled)
                            Text(isDriverInstalled ? "Installed" : "Not Installed")
                                .foregroundStyle(.secondary)
                        }
                    }
                    HStack {
                        Spacer()
                        if isBusy {
                            ProgressView().controlSize(.small)
                        }
                        if isDriverInstalled {
                            Button("Uninstall…", role: .destructive) { uninstallDriver() }
                        } else {
                            Button("Install…") { installDriver() }
                        }
                    }
                }

                Section {
                    Picker("Microphone", selection: $selectedMicUID) {
                        Text("System Default").tag(String?.none)
                        ForEach(availableMics, id: \.uid) { mic in
                            Text(mic.name).tag(String?.some(mic.uid))
                        }
                    }
                    .disabled(audioEngine.isCallModeBusy)
                    .onChange(of: selectedMicUID) { _, newValue in
                        audioEngine.selectedMicUID = newValue
                        if audioEngine.isCallModeEnabled {
                            audioEngine.setCallModeEnabled(true, micDeviceUID: newValue)
                        }
                    }
                } footer: {
                    if audioEngine.isCallModeBusy {
                        HStack(spacing: 6) {
                            ProgressView().controlSize(.small)
                            Text("Switching microphone…")
                        }
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                    } else if selectedMicUID != nil {
                        Text("Becomes your Mac's system input while Call Mode is on.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                }

                Section {
                    Toggle("Route to Call Mode", isOn: Binding(
                        get: { audioEngine.isCallModeEnabled },
                        set: { audioEngine.setCallModeEnabled($0, micDeviceUID: selectedMicUID) }
                    ))
                    .disabled(!isDriverInstalled || audioEngine.isCallModeBusy)
                } footer: {
                    statusFooter
                }
            }
            .formStyle(.grouped)
            .fixedSize(horizontal: false, vertical: true)

            if let actionError {
                Label(actionError, systemImage: "exclamationmark.triangle.fill")
                    .font(.footnote)
                    .foregroundStyle(.red)
            }

        }
        .padding(20)
        .frame(width: 420)
        .onAppear {
            isDriverInstalled = LoopbackDriver.isInstalled
            availableMics = AudioDeviceDirectory.inputCapableDevices(excludingUIDs: [LoopbackDriver.deviceUID])
            selectedMicUID = audioEngine.selectedMicUID
        }
    }

    private func statusIcon(_ ok: Bool) -> some View {
        Image(systemName: ok ? "checkmark.circle.fill" : "xmark.circle")
            .foregroundStyle(ok ? .green : .secondary)
    }

    @ViewBuilder
    private var statusFooter: some View {
        VStack(alignment: .trailing, spacing: 4) {
            Text(footerLeadingText)
                .foregroundStyle(audioEngine.callModeError == nil ? Color.secondary : Color.red)
                .multilineTextAlignment(.trailing)

            if audioEngine.callModeError == nil, audioEngine.isCallModeEnabled {
                Label("Connected", systemImage: "checkmark.circle.fill")
                    .foregroundStyle(.green)
            }
        }
        .frame(maxWidth: .infinity, alignment: .trailing)
        .font(.footnote)
    }

    private var footerLeadingText: String {
        if let error = audioEngine.callModeError {
            return error
        } else if audioEngine.isCallModeEnabled {
            return "Select \"Syrinx Microphone\" as your input in your call app."
        } else {
            return "Sends your mic + pads to Syrinx Microphone, mixed together."
        }
    }

    private func installDriver() {
        isBusy = true
        actionError = nil
        LoopbackDriver.install { result in
            isBusy = false
            switch result {
            case .success:
                isDriverInstalled = LoopbackDriver.isInstalled
            case .failure(let error):
                actionError = error.localizedDescription
            }
        }
    }

    private func uninstallDriver() {
        isBusy = true
        actionError = nil
        audioEngine.setCallModeEnabled(false)
        LoopbackDriver.uninstall { result in
            isBusy = false
            switch result {
            case .success:
                isDriverInstalled = LoopbackDriver.isInstalled
            case .failure(let error):
                actionError = error.localizedDescription
            }
        }
    }
}
