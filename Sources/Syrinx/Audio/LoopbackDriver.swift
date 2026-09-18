import CoreAudio
import Foundation

/// Detects and manages the Syrinx virtual audio devices (see
/// Driver/SyrinxLoopback.c at the repo root). The driver publishes
/// two device objects sharing one ring buffer: a visible input-side
/// device ("Syrinx Microphone", what call apps pick as their mic) and
/// a hidden output-side device (what `AudioEngine` renders into) -
/// see the driver's file doc comment for why it's split that way.
///
/// The driver is compiled from source and installed locally, on
/// demand, rather than shipped as a prebuilt binary: an ad-hoc-signed
/// HAL plugin *built on the user's own machine* loads without the
/// stricter Gatekeeper/notarization gate that applies to a
/// downloaded, prebuilt system extension — see the install-driver.sh
/// test that proved this out.
enum LoopbackDriver {
    /// UID of the visible input-side device - what call apps show as
    /// "Syrinx Microphone" in their microphone picker.
    static let deviceUID = "com.hojin.syrinx.loopback.device"

    /// UID of the hidden output-side device - what `AudioEngine` pins
    /// its render engine to. Never shown to users.
    static let outputDeviceUID = "com.hojin.syrinx.loopback.output.device"

    /// Returns the AudioDeviceID of the installed input-side ("Syrinx
    /// Microphone") device, or nil if it isn't installed/loaded.
    static func findDeviceID() -> AudioDeviceID? {
        AudioDeviceDirectory.deviceID(forUID: deviceUID)
    }

    /// Returns the AudioDeviceID of the installed hidden output-side
    /// device, or nil if it isn't installed/loaded.
    static func findOutputDeviceID() -> AudioDeviceID? {
        AudioDeviceDirectory.deviceID(forUID: outputDeviceUID)
    }

    static var isInstalled: Bool {
        findDeviceID() != nil
    }

    // MARK: - Install / Uninstall

    enum DriverActionError: LocalizedError {
        case bundleNotFound
        case commandLineToolsMissing
        case scriptFailed(String)

        var errorDescription: String? {
            switch self {
            case .bundleNotFound:
                return "Couldn't find the driver source bundled with Syrinx. Run the packaged .app, not a raw swift run build."
            case .commandLineToolsMissing:
                return "Syrinx needs Xcode Command Line Tools to build its audio driver. Open Terminal, run \"xcode-select --install\", then try again."
            case .scriptFailed(let output):
                return "Driver script failed:\n\(output)"
            }
        }
    }

    /// True if Xcode Command Line Tools are installed, so `clang` can
    /// actually compile the driver. Homebrew requires CLT as its own
    /// prerequisite, so users who install Syrinx via `brew` always
    /// have this — but someone who just drags a downloaded .app into
    /// Applications without ever touching Homebrew or Xcode might
    /// not. Checked up front so that case gets a clear, actionable
    /// message instead of a cryptic compile failure or macOS's own
    /// separate "install developer tools?" prompt appearing
    /// unexpectedly mid-install.
    private static var isCommandLineToolsInstalled: Bool {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/xcode-select")
        process.arguments = ["-p"]
        process.standardOutput = Pipe()
        process.standardError = Pipe()
        do {
            try process.run()
            process.waitUntilExit()
            return process.terminationStatus == 0
        } catch {
            return false
        }
    }

    /// The bundled driver source directory, copied into the .app by
    /// Scripts/build-app.sh at Contents/Resources/Driver.
    private static var bundledDriverDirectory: URL? {
        Bundle.main.resourceURL?.appendingPathComponent("Driver", isDirectory: true)
    }

    /// Compiles the driver fresh (ad-hoc signed, locally built) and
    /// installs it, prompting for admin credentials via the OS's
    /// native authorization dialog. Runs on a background queue;
    /// completion is called on the main queue.
    static func install(completion: @escaping (Result<Void, Error>) -> Void) {
        runScripts(["build-driver.sh", "install-driver.sh"], expectedInstalled: true, requiresCommandLineTools: true, completion: completion)
    }

    static func uninstall(completion: @escaping (Result<Void, Error>) -> Void) {
        runScripts(["uninstall-driver.sh"], expectedInstalled: false, requiresCommandLineTools: false, completion: completion)
    }

    private static func runScripts(_ scriptNames: [String], expectedInstalled: Bool, requiresCommandLineTools: Bool, completion: @escaping (Result<Void, Error>) -> Void) {
        guard let driverDirectory = bundledDriverDirectory,
              FileManager.default.fileExists(atPath: driverDirectory.path) else {
            DispatchQueue.main.async { completion(.failure(DriverActionError.bundleNotFound)) }
            return
        }

        DispatchQueue.global(qos: .userInitiated).async {
            if requiresCommandLineTools, !isCommandLineToolsInstalled {
                DispatchQueue.main.async { completion(.failure(DriverActionError.commandLineToolsMissing)) }
                return
            }

            for scriptName in scriptNames {
                let scriptURL = driverDirectory.appendingPathComponent("Scripts/\(scriptName)")
                let process = Process()
                process.executableURL = URL(fileURLWithPath: "/bin/bash")
                process.arguments = [scriptURL.path]
                process.currentDirectoryURL = driverDirectory

                let pipe = Pipe()
                process.standardOutput = pipe
                process.standardError = pipe

                do {
                    try process.run()
                } catch {
                    DispatchQueue.main.async { completion(.failure(error)) }
                    return
                }
                process.waitUntilExit()

                let outputData = pipe.fileHandleForReading.readDataToEndOfFile()
                let output = String(data: outputData, encoding: .utf8) ?? ""

                if process.terminationStatus != 0 {
                    DispatchQueue.main.async { completion(.failure(DriverActionError.scriptFailed(output))) }
                    return
                }
            }

            // Both scripts end with `killall coreaudiod`, which
            // restarts the audio daemon asynchronously — it returns
            // immediately without waiting for coreaudiod to come back
            // up and re-enumerate the driver's devices. Checking
            // `isInstalled` the instant the script exits can read a
            // stale answer, showing the wrong status until the Call
            // Mode panel happens to be reopened later. Poll for the
            // expected state with a bounded retry before reporting
            // success, so the UI reflects reality right away.
            for attempt in 0..<20 {
                if isInstalled == expectedInstalled { break }
                if attempt < 19 { Thread.sleep(forTimeInterval: 0.25) }
            }

            DispatchQueue.main.async { completion(.success(())) }
        }
    }
}
