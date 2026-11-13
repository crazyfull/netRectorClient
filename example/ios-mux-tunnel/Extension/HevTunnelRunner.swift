import Foundation
import HevSocks5Tunnel

/// Runs hev-socks5-tunnel (tun2socks) on its own thread: it moves IP packets between the utun
/// interface and the local SOCKS5 listener of the mux client.
final class HevTunnelRunner {
    private var thread: Thread?
    private var isStopping = false
    private let lock = NSLock()

    /// Starts the engine. `onExit` is called on the engine thread when it ends without stop()
    /// having been called.
    func start(configurationPath: String, tunnelDescriptor: Int32, onExit: @escaping (Int32) -> Void) {
        lock.lock()
        isStopping = false
        lock.unlock()

        let thread = Thread { [weak self] in
            let code = hev_socks5_tunnel_main_from_file(configurationPath, tunnelDescriptor)
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
        thread.name = "hev-socks5-tunnel"
        thread.qualityOfService = .userInitiated
        self.thread = thread
        thread.start()
    }

    /// Stops the engine and waits for its thread to finish.
    func stop() {
        lock.lock()
        isStopping = true
        lock.unlock()

        hev_socks5_tunnel_quit()

        let deadline = Date().addingTimeInterval(3)
        while let thread = thread, !thread.isFinished, Date() < deadline {
            Thread.sleep(forTimeInterval: 0.02)
        }
        thread = nil
    }
}
