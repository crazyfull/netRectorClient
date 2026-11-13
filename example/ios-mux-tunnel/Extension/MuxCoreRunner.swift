import Foundation
import MuxTunnelCore

/// Runs the mux-tunnelClient core (SOCKS5 listener + tunnels to the CDN) on its own thread.
final class MuxCoreRunner {
    private var thread: Thread?
    private var isStopping = false
    private let lock = NSLock()

    /// Tunnels that are connected right now (handshake done).
    var connectedTunnels: Int {
        return Int(muxTunnelCoreConnectedTunnels())
    }

    /// Starts the core. `onExit` is called on the core thread when it ends without stop() having
    /// been called (for example when the SOCKS5 port cannot be opened).
    func start(configuration: TunnelConfiguration, socksPort: UInt16, onExit: @escaping (Int32) -> Void) {
        lock.lock()
        isStopping = false
        lock.unlock()

        muxTunnelCoreReset()

        let thread = Thread { [weak self] in
            let code = MuxCoreRunner.run(configuration: configuration, socksPort: socksPort)
            guard let self = self else {
                return
            }
            self.lock.lock()
            let stoppedByUs = self.isStopping
            self.lock.unlock()
            if !stoppedByUs {
                onExit(code)
            }
        }
        thread.name = "mux-core"
        thread.qualityOfService = .userInitiated
        self.thread = thread
        thread.start()
    }

    /// Stops the core and waits for its thread to finish.
    func stop() {
        lock.lock()
        isStopping = true
        lock.unlock()

        muxTunnelCoreStop()

        let deadline = Date().addingTimeInterval(3)
        while let thread = thread, !thread.isFinished, Date() < deadline {
            Thread.sleep(forTimeInterval: 0.02)
        }
        thread = nil
    }

    private static func run(configuration: TunnelConfiguration, socksPort: UInt16) -> Int32 {
        // The C strings must stay valid for the whole run
        let strings = [
            DefaultTunnelSettings.socksAddress,
            configuration.cdnAddress.trimmingCharacters(in: .whitespaces),
            configuration.handshakeHost,
            configuration.handshakePath,
            configuration.webSocketKey,
            configuration.expectedAccept
        ].map { (text: String) -> UnsafeMutablePointer<CChar> in
            guard let copy = strdup(text) else {
                fatalError("out of memory")
            }
            return copy
        }
        defer { strings.forEach { free($0) } }

        var core = MuxTunnelCoreConfig(
            listenAddress: UnsafePointer(strings[0]),
            listenPort: socksPort,
            cdnAddress: UnsafePointer(strings[1]),
            cdnPort: UInt16(configuration.cdnPort),
            tunnelCount: Int32(configuration.tunnelCount),
            handshakeHost: UnsafePointer(strings[2]),
            handshakePath: UnsafePointer(strings[3]),
            webSocketKey: UnsafePointer(strings[4]),
            expectedAccept: UnsafePointer(strings[5]),
            keepAliveSeconds: DefaultTunnelSettings.keepAliveSeconds,
            verbose: 0)

        return muxTunnelCoreRun(&core)
    }
}
