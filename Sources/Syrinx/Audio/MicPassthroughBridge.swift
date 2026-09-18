import AVFoundation
import AudioToolbox

/// Bridges live microphone audio into Call Mode's loopback mix.
///
/// One AUHAL-backed `AVAudioEngine` can only target a single device
/// for both its input and output scopes — so capturing the real mic
/// while simultaneously outputting to Syrinx Loopback needs two
/// independent engines. This runs a dedicated capture-only engine on
/// the mic, converts each buffer to the loopback engine's format, and
/// feeds it through a small ring buffer into an `AVAudioSourceNode`
/// that the loopback engine's mixer pulls from — so a voice call
/// hears the user's mic and pad audio mixed together.
final class MicPassthroughBridge {
    private let captureEngine = AVAudioEngine()
    private let targetFormat: AVAudioFormat
    private var converter: AVAudioConverter?

    private let lock = NSLock()
    private var leftBuffer: [Float] = []
    private var rightBuffer: [Float] = []
    private let maxBufferedFrames: Int

    init(targetFormat: AVAudioFormat, bufferSeconds: Double = 0.5) {
        self.targetFormat = targetFormat
        self.maxBufferedFrames = Int(targetFormat.sampleRate * bufferSeconds)
    }

    enum StartError: LocalizedError {
        case invalidHardwareFormat

        var errorDescription: String? {
            "the input device reported an invalid format (0 channels or 0Hz) — it may still be connecting"
        }
    }

    /// Starts capturing from the current system default input
    /// device. Caller is responsible for having already confirmed
    /// mic authorization, and for having already pointed the system
    /// default input at the desired device beforehand if a specific
    /// (non-default) mic is wanted — pinning this capture engine's
    /// input directly to an arbitrary device via
    /// `kAudioOutputUnitProperty_CurrentDevice` raced with AVAudioEngine's
    /// format negotiation and crashed with "Input HW format and tap
    /// format not matching"; always following system default reuses
    /// the one code path that's actually proven reliable.
    func start() throws {
        stop()

        // This engine exists purely to tap the mic — it must never
        // audibly echo back to the user's own speakers/headphones.
        // AVAudioEngine leaves output IO enabled by default even
        // though nothing is ever connected to it, and on many
        // devices that's enough for the hardware/driver to perform
        // its own direct input-to-output monitoring pass-through,
        // completely bypassing this software graph. Disabling output
        // IO explicitly stops that.
        if let outputUnit = captureEngine.outputNode.audioUnit {
            var disable: UInt32 = 0
            AudioUnitSetProperty(
                outputUnit,
                kAudioOutputUnitProperty_EnableIO,
                kAudioUnitScope_Output,
                0,
                &disable,
                UInt32(MemoryLayout<UInt32>.size)
            )
        }

        let inputFormat = captureEngine.inputNode.outputFormat(forBus: 0)

        // A device that's still finishing becoming the default input
        // (e.g. an iPhone's Continuity mic mid-handshake) can briefly
        // report a degenerate 0-channel/0Hz format. `installTapOnBus`
        // raises an *Objective-C* exception for that — not a catchable
        // Swift `Error` — so it must be rejected here, before ever
        // reaching installTap, rather than caught afterward.
        guard inputFormat.channelCount > 0, inputFormat.sampleRate > 0 else {
            throw StartError.invalidHardwareFormat
        }

        converter = AVAudioConverter(from: inputFormat, to: targetFormat)

        captureEngine.inputNode.installTap(onBus: 0, bufferSize: 1024, format: inputFormat) { [weak self] buffer, _ in
            self?.ingest(buffer)
        }
        try captureEngine.start()
    }

    func stop() {
        captureEngine.inputNode.removeTap(onBus: 0)
        if captureEngine.isRunning {
            captureEngine.stop()
        }
        lock.lock()
        leftBuffer.removeAll()
        rightBuffer.removeAll()
        lock.unlock()
    }

    /// Creates the node to attach+connect into the destination
    /// (loopback) engine's mixer to receive the bridged mic audio.
    func makeSourceNode() -> AVAudioSourceNode {
        AVAudioSourceNode(format: targetFormat) { [weak self] _, _, frameCount, audioBufferList in
            self?.render(frameCount: Int(frameCount), into: audioBufferList) ?? noErr
        }
    }

    private func ingest(_ buffer: AVAudioPCMBuffer) {
        guard let converter else { return }
        let ratio = targetFormat.sampleRate / buffer.format.sampleRate
        let outCapacity = AVAudioFrameCount(Double(buffer.frameLength) * ratio) + 16
        guard let converted = AVAudioPCMBuffer(pcmFormat: targetFormat, frameCapacity: outCapacity) else { return }

        var consumed = false
        var convError: NSError?
        converter.convert(to: converted, error: &convError) { _, outStatus in
            if consumed {
                outStatus.pointee = .noDataNow
                return nil
            }
            consumed = true
            outStatus.pointee = .haveData
            return buffer
        }
        guard convError == nil, let channels = converted.floatChannelData else { return }

        let frameCount = Int(converted.frameLength)
        let sourceChannelCount = Int(converted.format.channelCount)

        lock.lock()
        for i in 0..<frameCount {
            leftBuffer.append(channels[0][i])
            rightBuffer.append(sourceChannelCount > 1 ? channels[1][i] : channels[0][i])
        }
        let overflow = leftBuffer.count - maxBufferedFrames
        if overflow > 0 {
            leftBuffer.removeFirst(overflow)
            rightBuffer.removeFirst(overflow)
        }
        lock.unlock()
    }

    private func render(frameCount: Int, into audioBufferList: UnsafeMutablePointer<AudioBufferList>) -> OSStatus {
        let abl = UnsafeMutableAudioBufferListPointer(audioBufferList)

        lock.lock()
        let available = min(frameCount, leftBuffer.count)
        let left = Array(leftBuffer.prefix(available))
        let right = Array(rightBuffer.prefix(available))
        if available > 0 {
            leftBuffer.removeFirst(available)
            rightBuffer.removeFirst(available)
        }
        lock.unlock()

        if abl.count > 0, let dst0 = abl[0].mData?.assumingMemoryBound(to: Float.self) {
            for i in 0..<frameCount { dst0[i] = i < left.count ? left[i] : 0 }
        }
        if abl.count > 1, let dst1 = abl[1].mData?.assumingMemoryBound(to: Float.self) {
            for i in 0..<frameCount { dst1[i] = i < right.count ? right[i] : 0 }
        }
        return noErr
    }
}
