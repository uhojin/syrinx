@preconcurrency import AVFoundation
import AppKit
import AudioToolbox
import CoreAudio
import Observation

/// One AVAudioPlayerNode per active playback (not per pad), so the same
/// pad can overlap itself and any number of different pads can play at
/// once — a single player node can't overlap its own playback, but the
/// engine happily mixes any number of nodes together for free.
///
/// When Call Mode is on, each play() also renders to a second engine
/// pinned to the hidden output-side device (see LoopbackDriver), so
/// pad audio reaches a voice call while still playing locally. This
/// never touches the real microphone — Syrinx only ever opens output
/// nodes.
///
/// `@Observable` (not `ObservableObject`/`@Published`) so a view
/// reading only `isCallModeEnabled` (say) doesn't re-render every
/// time `playingPadIDs` changes, and vice versa.
@Observable
@MainActor
final class AudioEngine {
    private(set) var playingPadIDs: Set<UUID> = []
    private(set) var isCallModeEnabled: Bool = false
    private(set) var callModeError: String?
    private(set) var activeMicName: String?
    /// True while a Call Mode enable/mic-switch is in flight — lets
    /// the UI show progress instead of looking unresponsive during
    /// the (now non-blocking) device-settle wait.
    private(set) var isCallModeBusy: Bool = false
    private let engine = AVAudioEngine()
    private var loopbackEngine: AVAudioEngine?
    private var micBridge: MicPassthroughBridge?
    private var micSourceNode: AVAudioSourceNode?
    /// The system default input device as it was before Call Mode
    /// redirected it to a specifically-chosen mic, so it can be
    /// restored when Call Mode turns off or switches back to
    /// "System Default".
    private var originalDefaultInputDeviceID: AudioDeviceID?

    #if DEBUG
    /// Exposed only for standalone verification scripts (see /tmp
    /// smoke tests during development) — never used by app UI.
    var debugLocalMixerNode: AVAudioMixerNode { engine.mainMixerNode }
    #endif

    private struct Playback {
        var padID: UUID
        var nodes: [(engine: AVAudioEngine, node: AVAudioPlayerNode)]
        var finishedNodeCount: Int = 0
    }
    private var activePlaybacks: [UUID: Playback] = [:]

    // Deliberately no eager `prepare()`/`start()` here: AVAudioEngine's
    // graph has no I/O configured until at least one node is attached
    // and connected, and starting before that throws
    // "inputNode != nullptr || outputNode != nullptr". `play()` starts
    // the engine lazily right after the first connection.
    init() {}

    func play(pad: Pad) {
        guard let url = pad.fileURL else { return }

        var targets: [AVAudioEngine] = [engine]
        if isCallModeEnabled, let loopbackEngine {
            targets.append(loopbackEngine)
        }

        let playbackID = UUID()
        var nodes: [(engine: AVAudioEngine, node: AVAudioPlayerNode)] = []

        for target in targets {
            // Each target needs its own AVAudioFile: the file object
            // owns a stateful read cursor that isn't safe to schedule
            // from two nodes concurrently.
            guard let file = try? AVAudioFile(forReading: url) else {
                NSSound.beep()
                print("Syrinx: could not open audio file at \(url.path)")
                continue
            }

            let node = AVAudioPlayerNode()
            target.attach(node)
            target.connect(node, to: target.mainMixerNode, format: file.processingFormat)
            node.volume = pad.volume
            nodes.append((target, node))

            if !target.isRunning {
                try? target.start()
            }

            node.scheduleFile(file, at: nil, completionCallbackType: .dataPlayedBack) { [weak self] _ in
                DispatchQueue.main.async {
                    self?.finishPlaybackNode(playbackID: playbackID, padID: pad.id, engine: target, node: node)
                }
            }
        }

        guard !nodes.isEmpty else { return }

        activePlaybacks[playbackID] = Playback(padID: pad.id, nodes: nodes)
        playingPadIDs.insert(pad.id)

        for (_, node) in nodes {
            node.play()
        }
    }

    func stopAll() {
        for (_, playback) in activePlaybacks {
            for (nodeEngine, node) in playback.nodes {
                node.stop()
                nodeEngine.detach(node)
            }
        }
        activePlaybacks.removeAll()
        playingPadIDs.removeAll()
    }

    private func finishPlaybackNode(playbackID: UUID, padID: UUID, engine nodeEngine: AVAudioEngine, node: AVAudioPlayerNode) {
        guard var playback = activePlaybacks[playbackID] else { return }
        nodeEngine.detach(node)
        playback.finishedNodeCount += 1

        if playback.finishedNodeCount >= playback.nodes.count {
            activePlaybacks.removeValue(forKey: playbackID)
        } else {
            activePlaybacks[playbackID] = playback
        }

        if !activePlaybacks.values.contains(where: { $0.padID == padID }) {
            playingPadIDs.remove(padID)
        }
    }

    // MARK: - Call Mode

    private static let selectedMicUIDDefaultsKey = "AudioEngine.selectedMicUID"
    private static let isCallModeEnabledDefaultsKey = "AudioEngine.isCallModeEnabled"

    /// The real input device UID to use for Call Mode, or nil to
    /// follow the system default input. Persisted across launches.
    var selectedMicUID: String? {
        get { UserDefaults.standard.string(forKey: Self.selectedMicUIDDefaultsKey) }
        set { UserDefaults.standard.set(newValue, forKey: Self.selectedMicUIDDefaultsKey) }
    }

    /// Enables/disables routing pad audio *and the user's live mic*
    /// to the hidden output-side device, mixed together, in addition
    /// to normal local pad playback. Requests microphone access on
    /// first use. `micDeviceUID` pins a specific input device;
    /// omit/nil to follow the system default input. Sets
    /// `callModeError` if the driver isn't installed, mic access is
    /// denied, or the device can't be pinned as the engine's output.
    /// The actual setup runs asynchronously (`isCallModeBusy` reports
    /// progress; `isCallModeEnabled`/`callModeError` report the
    /// outcome) since it involves brief, non-blocking hardware-settle
    /// waits that would otherwise freeze the UI for up to ~1s.
    ///
    /// The resulting on/off state is persisted, so `restoreIfNeeded()`
    /// can bring Call Mode back on the next launch if it was on when
    /// the app last quit.
    func setCallModeEnabled(_ enabled: Bool, micDeviceUID: String? = nil) {
        guard enabled else {
            teardownLoopbackEngine()
            updateIsCallModeEnabled(false)
            callModeError = nil
            return
        }

        guard let inputDeviceID = LoopbackDriver.findDeviceID(),
              let outputDeviceID = LoopbackDriver.findOutputDeviceID() else {
            callModeError = "Syrinx Microphone isn't installed."
            updateIsCallModeEnabled(false)
            return
        }

        let micDeviceID = micDeviceUID.flatMap { AudioDeviceDirectory.deviceID(forUID: $0) }

        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized:
            Task { await self.activateCallMode(outputDeviceID: outputDeviceID, loopbackInputDeviceID: inputDeviceID, micDeviceID: micDeviceID) }
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .audio) { [weak self] granted in
                Task { @MainActor in
                    guard let self else { return }
                    if granted {
                        await self.activateCallMode(outputDeviceID: outputDeviceID, loopbackInputDeviceID: inputDeviceID, micDeviceID: micDeviceID)
                    } else {
                        self.callModeError = "Microphone access was denied. Enable it in System Settings > Privacy & Security > Microphone."
                        self.updateIsCallModeEnabled(false)
                    }
                }
            }
        case .denied, .restricted:
            callModeError = "Microphone access was denied. Enable it in System Settings > Privacy & Security > Microphone."
            updateIsCallModeEnabled(false)
        @unknown default:
            callModeError = "Unknown microphone authorization status."
            updateIsCallModeEnabled(false)
        }
    }

    /// Re-enables Call Mode on launch if it was still on when the app
    /// last quit. A no-op if it wasn't, or if setup fails (e.g. the
    /// driver got uninstalled since) — same error reporting as a
    /// manual toggle, just surfaced silently until Call Mode is opened.
    func restoreCallModeIfNeeded() {
        guard UserDefaults.standard.bool(forKey: Self.isCallModeEnabledDefaultsKey) else { return }
        setCallModeEnabled(true, micDeviceUID: selectedMicUID)
    }

    private func updateIsCallModeEnabled(_ enabled: Bool) {
        isCallModeEnabled = enabled
        UserDefaults.standard.set(enabled, forKey: Self.isCallModeEnabledDefaultsKey)
    }

    private func activateCallMode(outputDeviceID: AudioDeviceID, loopbackInputDeviceID: AudioDeviceID, micDeviceID: AudioDeviceID?) async {
        isCallModeBusy = true
        defer { isCallModeBusy = false }
        do {
            try await setUpLoopbackEngine(outputDeviceID: outputDeviceID, loopbackInputDeviceID: loopbackInputDeviceID, micDeviceID: micDeviceID)
            updateIsCallModeEnabled(true)
            callModeError = nil
        } catch {
            teardownLoopbackEngine()
            updateIsCallModeEnabled(false)
            callModeError = "Couldn't set up Call Mode: \(error.localizedDescription)"
        }
    }

    private enum LoopbackEngineError: LocalizedError {
        case noAudioUnit
        case setDeviceFailed(OSStatus)
        case selectedInputIsLoopback

        var errorDescription: String? {
            switch self {
            case .noAudioUnit:
                return "the engine's output has no underlying audio unit"
            case .setDeviceFailed(let status):
                return "AudioUnitSetProperty(CurrentDevice) failed (\(status))"
            case .selectedInputIsLoopback:
                return "The selected microphone is Syrinx Microphone itself. Capturing it would feed its own output back into its input in an endless loop. Pick a different input."
            }
        }
    }

    private func setUpLoopbackEngine(outputDeviceID: AudioDeviceID, loopbackInputDeviceID: AudioDeviceID, micDeviceID: AudioDeviceID?) async throws {
        // Decide + apply the mic redirect before touching the engine
        // at all.
        let targetMicDeviceID = try await redirectDefaultInput(toward: micDeviceID, loopbackDeviceID: loopbackInputDeviceID)

        // Always fully tear down and rebuild rather than trying to
        // reconfigure a live engine in place: pausing a running,
        // custom-device-pinned engine to swap just its mic source
        // left the WHOLE engine silent afterward — including pad
        // playback that should have been unaffected by a mic switch
        // at all. A brief rebuild glitch is a far safer tradeoff than
        // staying silently broken until Call Mode is toggled off/on.
        stopLoopbackEngineOnly()

        let newEngine = AVAudioEngine()

        guard let audioUnit = newEngine.outputNode.audioUnit else {
            throw LoopbackEngineError.noAudioUnit
        }

        var mutableDeviceID = outputDeviceID
        let status = AudioUnitSetProperty(
            audioUnit,
            kAudioOutputUnitProperty_CurrentDevice,
            kAudioUnitScope_Global,
            0,
            &mutableDeviceID,
            UInt32(MemoryLayout<AudioDeviceID>.size)
        )
        guard status == noErr else {
            throw LoopbackEngineError.setDeviceFailed(status)
        }

        // Bridge the real mic into this engine's mix so a voice call
        // hears the user's voice and pad audio combined — a plain
        // Aggregate Device can't do this: it only concatenates
        // sub-devices' channels side by side rather than mixing them,
        // so a normal mono/stereo-capturing call app only ever hears
        // whichever sub-device landed on the first channel(s).
        let micFormat = AVAudioFormat(standardFormatWithSampleRate: 48000, channels: 2)!
        let bridge = MicPassthroughBridge(targetFormat: micFormat)

        // Some devices (e.g. an iPhone's Continuity mic, or switching
        // to built-in hardware right after changing default input)
        // need a brief moment to finish becoming usable; retry
        // several times with backoff before surfacing an error.
        var startError: Error?
        var didStart = false
        for attempt in 0..<5 {
            do {
                try bridge.start()
                didStart = true
                break
            } catch {
                startError = error
                if attempt < 4 {
                    try? await Task.sleep(nanoseconds: 200_000_000)
                }
            }
        }
        guard didStart else {
            throw startError ?? LoopbackEngineError.noAudioUnit
        }

        let sourceNode = bridge.makeSourceNode()
        newEngine.attach(sourceNode)
        newEngine.connect(sourceNode, to: newEngine.mainMixerNode, format: micFormat)

        micBridge = bridge
        micSourceNode = sourceNode
        activeMicName = targetMicDeviceID.flatMap { AudioDeviceDirectory.name(for: $0) }

        // Unlike the lazy-start pattern elsewhere in this file, the
        // graph is non-empty right now (mic source node is already
        // connected), so starting immediately is safe.
        try newEngine.start()

        loopbackEngine = newEngine
    }

    /// Points system default input at the chosen mic (or restores the
    /// original if switching back to "System Default"), returning the
    /// resulting effective device. Throws if that would be the
    /// loopback device itself (feedback loop).
    private func redirectDefaultInput(toward micDeviceID: AudioDeviceID?, loopbackDeviceID: AudioDeviceID) async throws -> AudioDeviceID? {
        let targetMicDeviceID: AudioDeviceID?
        var didChangeDefaultInput = false
        if let micDeviceID {
            if originalDefaultInputDeviceID == nil {
                originalDefaultInputDeviceID = AudioDeviceDirectory.defaultInputDeviceID()
            }
            if AudioDeviceDirectory.defaultInputDeviceID() != micDeviceID {
                AudioDeviceDirectory.setDefaultInputDevice(micDeviceID)
                didChangeDefaultInput = true
            }
            targetMicDeviceID = micDeviceID
        } else if let original = originalDefaultInputDeviceID {
            AudioDeviceDirectory.setDefaultInputDevice(original)
            originalDefaultInputDeviceID = nil
            targetMicDeviceID = original
            didChangeDefaultInput = true
        } else {
            targetMicDeviceID = AudioDeviceDirectory.defaultInputDeviceID()
        }

        // Refuse if the effective default input is now the Syrinx
        // Microphone device itself — capturing it would feed its own
        // output straight back into itself every render cycle, an
        // endless feedback loop rather than picking up a real mic.
        guard targetMicDeviceID != loopbackDeviceID else {
            throw LoopbackEngineError.selectedInputIsLoopback
        }

        // Switching default input — especially to built-in hardware —
        // can take CoreAudio a moment to physically reconfigure before
        // a freshly created AVAudioEngine can successfully open it.
        // Give it a brief head start before even attempting to bridge.
        if didChangeDefaultInput {
            try? await Task.sleep(nanoseconds: 200_000_000)
        }

        return targetMicDeviceID
    }

    /// Stops just the audio graph, without touching the default-input
    /// redirect — used both for a full Call Mode teardown (followed
    /// by restoring default input) and mid-rebuild when a new target
    /// is about to be set immediately after.
    private func stopLoopbackEngineOnly() {
        if let engine = loopbackEngine {
            // Any pad playback nodes still scheduled on this engine
            // are about to be forcibly killed rather than naturally
            // finishing, so their `scheduleFile` completion handler
            // will never fire. Without finalizing them here,
            // `activePlaybacks`/`playingPadIDs` bookkeeping for those
            // pads gets stuck forever — observed as a pad staying
            // highlighted indefinitely after Call Mode reconfigures
            // (e.g. a mic switch) while it was mid-playback.
            let orphaned = activePlaybacks.compactMap { (playbackID, playback) -> (UUID, UUID, AVAudioPlayerNode)? in
                guard let node = playback.nodes.first(where: { $0.engine === engine })?.node else { return nil }
                return (playbackID, playback.padID, node)
            }
            for (playbackID, padID, node) in orphaned {
                finishPlaybackNode(playbackID: playbackID, padID: padID, engine: engine, node: node)
            }

            if let sourceNode = micSourceNode {
                engine.disconnectNodeOutput(sourceNode)
                engine.detach(sourceNode)
            }
        }
        micSourceNode = nil
        micBridge?.stop()
        micBridge = nil
        loopbackEngine?.stop()
        loopbackEngine = nil
        activeMicName = nil
    }

    private func teardownLoopbackEngine() {
        stopLoopbackEngineOnly()
        if let original = originalDefaultInputDeviceID {
            AudioDeviceDirectory.setDefaultInputDevice(original)
            originalDefaultInputDeviceID = nil
        }
    }
}
