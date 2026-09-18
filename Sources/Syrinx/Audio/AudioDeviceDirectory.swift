import AudioToolbox
import CoreAudio
import Foundation

/// Small shared helpers for listing/looking-up CoreAudio devices by
/// UID. Used by LoopbackDriver (find the virtual device) and
/// AudioEngine (report which mic Call Mode is currently using).
enum AudioDeviceDirectory {
    static func allDeviceIDs() -> [AudioDeviceID] {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var dataSize: UInt32 = 0
        var status = AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &dataSize)
        guard status == noErr, dataSize > 0 else { return [] }
        let count = Int(dataSize) / MemoryLayout<AudioDeviceID>.size
        var deviceIDs = [AudioDeviceID](repeating: 0, count: count)
        status = AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &dataSize, &deviceIDs)
        guard status == noErr else { return [] }
        return deviceIDs
    }

    static func uid(for deviceID: AudioDeviceID) -> String? {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyDeviceUID,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var uid: Unmanaged<CFString>?
        var uidSize = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        let status = withUnsafeMutablePointer(to: &uid) { pointer -> OSStatus in
            AudioObjectGetPropertyData(deviceID, &address, 0, nil, &uidSize, pointer)
        }
        guard status == noErr, let uid else { return nil }
        return uid.takeRetainedValue() as String
    }

    static func name(for deviceID: AudioDeviceID) -> String? {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioObjectPropertyName,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var name: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        let status = withUnsafeMutablePointer(to: &name) { pointer -> OSStatus in
            AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, pointer)
        }
        guard status == noErr, let name else { return nil }
        // Some devices (observed on a USB audio interface) report
        // their name with trailing padding whitespace baked in,
        // which visibly widens/misaligns picker rows showing it.
        return (name.takeRetainedValue() as String).trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// Looks up a device directly by UID via CoreAudio's dedicated
    /// translation property, rather than enumerating `allDeviceIDs()`
    /// and filtering — `kAudioHardwarePropertyDevices` (what
    /// `allDeviceIDs()` uses) silently excludes hidden devices, which
    /// made this unable to ever find Syrinx's own hidden output
    /// device. Direct UID translation isn't subject to that filter.
    static func deviceID(forUID uid: String) -> AudioDeviceID? {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyTranslateUIDToDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var uidRef = uid as CFString
        var deviceID = AudioDeviceID(kAudioObjectUnknown)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        let status = withUnsafeMutablePointer(to: &uidRef) { uidPtr in
            AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, UInt32(MemoryLayout<CFString>.size), uidPtr, &size, &deviceID)
        }
        guard status == noErr, deviceID != kAudioObjectUnknown else { return nil }
        return deviceID
    }

    /// The system's current default input device, i.e. the mic
    /// `AVAudioEngine.inputNode` captures from unless overridden.
    static func defaultInputDeviceID() -> AudioDeviceID? {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultInputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var deviceID: AudioDeviceID = 0
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        let status = AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &deviceID)
        guard status == noErr, deviceID != 0, deviceID != kAudioObjectUnknown else { return nil }
        return deviceID
    }

    static func defaultInputDeviceName() -> String? {
        guard let id = defaultInputDeviceID() else { return nil }
        return name(for: id)
    }

    /// Changes the system's default input device. Used by Call Mode
    /// to pin a specific mic: rather than pinning an `AVAudioEngine`
    /// directly to an arbitrary device (which crashed — see
    /// MicPassthroughBridge), Syrinx redirects system default input
    /// itself and relies on the engine's normal, proven-reliable
    /// default-follows-system behavior.
    @discardableResult
    static func setDefaultInputDevice(_ deviceID: AudioDeviceID) -> Bool {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultInputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var mutableDeviceID = deviceID
        let status = AudioObjectSetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &address,
            0, nil,
            UInt32(MemoryLayout<AudioDeviceID>.size),
            &mutableDeviceID
        )
        return status == noErr
    }

    static func inputChannelCount(for deviceID: AudioDeviceID) -> Int {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyStreamConfiguration,
            mScope: kAudioObjectPropertyScopeInput,
            mElement: kAudioObjectPropertyElementMain
        )
        var dataSize: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(deviceID, &address, 0, nil, &dataSize) == noErr, dataSize > 0 else { return 0 }
        let bufferListPointer = UnsafeMutableRawPointer.allocate(byteCount: Int(dataSize), alignment: MemoryLayout<AudioBufferList>.alignment)
        defer { bufferListPointer.deallocate() }
        guard AudioObjectGetPropertyData(deviceID, &address, 0, nil, &dataSize, bufferListPointer) == noErr else { return 0 }
        let bufferList = bufferListPointer.assumingMemoryBound(to: AudioBufferList.self)
        let buffers = UnsafeMutableAudioBufferListPointer(bufferList)
        return buffers.reduce(0) { $0 + Int($1.mNumberChannels) }
    }

    /// Real input devices for a Call Mode mic picker. Excludes
    /// Syrinx's own virtual Loopback device — selecting it as the mic
    /// would feed its own output back into itself.
    static func inputCapableDevices(excludingUIDs: Set<String> = []) -> [(uid: String, name: String, deviceID: AudioDeviceID)] {
        allDeviceIDs().compactMap { id in
            guard inputChannelCount(for: id) > 0,
                  let uid = uid(for: id),
                  !excludingUIDs.contains(uid),
                  let name = name(for: id)
            else { return nil }
            return (uid, name, id)
        }
    }
}
