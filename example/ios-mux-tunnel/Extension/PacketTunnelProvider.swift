import Foundation
import HevSocks5Tunnel
import MuxTunnelCore
import NetworkExtension

/// The Packet Tunnel extension: the VPN itself. It runs the mux client and the tun2socks engine
/// and answers the app's status requests.
///
///     apps -> utun -> hev-socks5-tunnel -> SOCKS5 127.0.0.1:port -> mux client -> CDN -> server
final class PacketTunnelProvider: NEPacketTunnelProvider {
    private let muxCore = MuxCoreRunner()
    private let tunnelEngine = HevTunnelRunner()
    private var configuration: TunnelConfiguration?
    private var statsTimer: DispatchSourceTimer?
    private let engineLogReader = HevLogReader()

    // The state is written from the provider queue and from the engine threads
    private let stateLock = NSLock()
    private var storedState = TunnelConnectionState.stopped
    private var storedFailure: Error?
    /// Why the engines stopped (the first failure), for the start that is still waiting.
    private var lastFailure: Error? {
        get {
            stateLock.lock()
            defer { stateLock.unlock() }
            return storedFailure
        }
        set {
            stateLock.lock()
            storedFailure = newValue
            stateLock.unlock()
        }
    }
    private var state: TunnelConnectionState {
        get {
            stateLock.lock()
            defer { stateLock.unlock() }
            return storedState
        }
        set {
            stateLock.lock()
            storedState = newValue
            stateLock.unlock()
        }
    }

    // MARK: - NEPacketTunnelProvider

    override func startTunnel(options: [String: NSObject]?, completionHandler: @escaping (Error?) -> Void) {
        state = .starting
        lastFailure = nil
        muxTunnelCoreSetLogFunction { level, message in
            if let message = message {
                DiagnosticLog.shared.add("core[\(level)] \(String(cString: message))")
            }
        }
        DiagnosticLog.shared.add("startTunnel, build \(DefaultTunnelSettings.buildStamp)")

        do {
            var configuration = try loadConfiguration()
            try configuration.validate()

            // Resolve the server now: later the system DNS points into the tunnel
            let host = configuration.cdnAddress.trimmingCharacters(in: .whitespaces)
            guard let address = ServerAddressResolver.resolve(host) else {
                throw TunnelProviderError.cannotResolveServer(host: host)
            }
            DiagnosticLog.shared.add("server \(host) -> \(address):\(configuration.cdnPort), tunnels \(configuration.tunnelCount)")
            configuration.cdnAddress = address
            ConnectivityProbe.run(label: "before vpn", address: address, port: UInt16(configuration.cdnPort))
            self.configuration = configuration

            guard let endpoint = LoopbackPortFinder.findEndpoint() else {
                throw TunnelProviderError.noFreePort
            }
            let socksPort = endpoint.port
            let engineAddress = endpoint.engineAddress
            let configurationPath = try HevConfigurationBuilder.writeFile(socksPort: socksPort, socksAddress: engineAddress)
            DiagnosticLog.shared.add("socks5 listener port \(socksPort), the engine connects to \(engineAddress)")

            setTunnelNetworkSettings(TunnelNetworkSettingsFactory.makeSettings(excludingServerAddress: address)) { [weak self] error in
                guard let self = self else {
                    return
                }
                if let error = error {
                    DiagnosticLog.shared.add("setTunnelNetworkSettings failed: \(error.localizedDescription)")
                    self.stopEngines()
                    self.state = .failed
                    completionHandler(error)
                    return
                }
                DiagnosticLog.shared.add("network settings applied, server \(address) excluded from the routes")
                ConnectivityProbe.run(label: "after vpn", address: address, port: UInt16(configuration.cdnPort))

                // The core connects to the server only now: the routes (with the server excluded) are in place
                self.muxCore.start(configuration: configuration, socksPort: socksPort) { [weak self] code in
                    DiagnosticLog.shared.add("mux core stopped, code \(code)")
                    self?.fail(with: TunnelProviderError.muxClientStopped(code: code))
                }

                // Is the local SOCKS5 listener reachable from this process? (hev connects to it)
                for delay in [1.0, 8.0] {
                    DispatchQueue.global().asyncAfter(deadline: .now() + delay) {
                        ConnectivityProbe.run(label: "socks5 listener after \(Int(delay)) s", address: DefaultTunnelSettings.socksAddress, port: socksPort)
                        // the engine's own way (IPv4-mapped, needs the hev patch) and the native IPv6 loopback
                        ConnectivityProbe.runLoopbackLikeEngine(label: "engine style after \(Int(delay)) s", address: "::ffff:127.0.0.1", port: socksPort)
                        ConnectivityProbe.runLoopbackLikeEngine(label: "IPv6 loopback after \(Int(delay)) s", address: "::1", port: socksPort)
                    }
                }
                // The VPN is announced (completionHandler) and the engine started only when the mux tunnel
                // is up; otherwise the first connections would be rejected ("no tunnel available")
                self.waitForMuxTunnel { [weak self] error in
                    guard let self = self else {
                        return
                    }
                    if let error = error {
                        DiagnosticLog.shared.add("start failed: \(error.localizedDescription)")
                        self.stopEngines()
                        self.state = .failed
                        completionHandler(error)
                        return
                    }
                    self.startTunnelEngine(configurationPath: configurationPath, completionHandler: completionHandler)
                }
            }
        } catch {
            DiagnosticLog.shared.add("start failed: \(error.localizedDescription)")
            muxCore.stop()
            state = .failed
            completionHandler(error)
        }
    }

    override func stopTunnel(with reason: NEProviderStopReason, completionHandler: @escaping () -> Void) {
        stopEngines()
        state = .stopped
        completionHandler()
    }

    override func handleAppMessage(_ messageData: Data, completionHandler: ((Data?) -> Void)?) {
        guard let completionHandler = completionHandler else {
            return
        }
        guard let request = try? TunnelProviderRequest.decoded(from: messageData) else {
            completionHandler(nil)
            return
        }

        switch request {
        case .status:
            completionHandler(try? currentStatus().encoded())
        case .log:
            completionHandler(diagnosticText().data(using: .utf8))
        }
    }

    // MARK: - Private

    private func loadConfiguration() throws -> TunnelConfiguration {
        guard let protocolConfiguration = protocolConfiguration as? NETunnelProviderProtocol,
              let data = protocolConfiguration.providerConfiguration?[TunnelConfigurationKey.configuration] as? Data else {
            throw TunnelProviderError.missingConfiguration
        }
        return try TunnelConfiguration.decoded(from: data)
    }

    /// Waits until the mux client has a connected tunnel. Ends with an error when the client stops
    /// or the server does not answer in time.
    private func waitForMuxTunnel(timeout: TimeInterval = 15, completion: @escaping (Error?) -> Void) {
        let deadline = Date().addingTimeInterval(timeout)
        DispatchQueue.global().async { [weak self] in
            while let self = self {
                if self.muxCore.connectedTunnels > 0 {
                    DiagnosticLog.shared.add("mux tunnel is up, starting the engine")
                    completion(nil)
                    return
                }
                if self.state == .failed {
                    completion(self.lastFailure ?? TunnelProviderError.muxClientStopped(code: -1))
                    return
                }
                if Date() >= deadline {
                    completion(TunnelProviderError.serverNotAnswering)
                    return
                }
                Thread.sleep(forTimeInterval: 0.05)
            }
        }
    }

    private func startTunnelEngine(configurationPath: String, completionHandler: @escaping (Error?) -> Void) {
        guard let descriptor = TunnelDescriptorLocator.locate(packetFlow: packetFlow) else {
            DiagnosticLog.shared.add("utun descriptor not found")
            stopEngines()
            state = .failed
            completionHandler(TunnelProviderError.noTunnelDescriptor)
            return
        }

        DiagnosticLog.shared.add("utun descriptor \(descriptor)")
        tunnelEngine.start(configurationPath: configurationPath, tunnelDescriptor: descriptor) { [weak self] code in
            DiagnosticLog.shared.add("tun2socks engine stopped, code \(code)")
            self?.fail(with: TunnelProviderError.tunnelEngineFailed)
        }
        state = .running
        startStatsTimer()
        DiagnosticLog.shared.add("tunnel running")
        completionHandler(nil)
    }

    private func currentStatus() -> TunnelStatus {
        return TunnelStatus(state: state,
                            connectedTunnels: state == .running ? muxCore.connectedTunnels : 0,
                            totalTunnels: configuration?.tunnelCount ?? 0)
    }

    /// The log plus the live numbers: packets moved through the utun interface tell where it stops.
    private func diagnosticText() -> String {
        let header = "state \(state), tunnels \(muxCore.connectedTunnels)/\(configuration?.tunnelCount ?? 0)\n"
            + "memory: \(Self.memoryFootprintMegabytes()) MB\n---\n"
        return header + DiagnosticLog.shared.text + "\n--- tun2socks engine log\n" + HevConfigurationBuilder.readLogTail()
    }

    private static func memoryFootprintMegabytes() -> Int {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size)
        let result = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        return result == KERN_SUCCESS ? Int(info.phys_footprint / (1024 * 1024)) : -1
    }

    /// Every second the status goes to the app; every third second a line with the packet counters
    /// (packets through the utun interface tell where traffic stops) goes into the log.
    private func startStatsTimer() {
        let timer = DispatchSource.makeTimerSource(queue: DispatchQueue.global())
        var round = 0
        timer.schedule(deadline: .now() + 1, repeating: 1)
        timer.setEventHandler { [weak self] in
            guard let self = self else {
                return
            }
            DiagnosticLog.shared.sendStatus(self.currentStatus())
            self.engineLogReader.readNewLines().forEach { DiagnosticLog.shared.add("hev: \($0)") }

            round += 1
            guard round % 3 == 0 else {
                return
            }
            var txPackets = 0, txBytes = 0, rxPackets = 0, rxBytes = 0
            hev_socks5_tunnel_stats(&txPackets, &txBytes, &rxPackets, &rxBytes)
            DiagnosticLog.shared.add("tick tunnels \(self.muxCore.connectedTunnels)/\(self.configuration?.tunnelCount ?? 0)"
                + " utun->engine \(txPackets)p/\(txBytes)B engine->utun \(rxPackets)p/\(rxBytes)B"
                + " mem \(Self.memoryFootprintMegabytes())MB")
        }
        timer.resume()
        statsTimer = timer
    }

    private func stopEngines() {
        statsTimer?.cancel()
        statsTimer = nil
        tunnelEngine.stop()
        muxCore.stop()
    }

    /// Called when one of the engines ends by itself: the VPN cannot work without both.
    private func fail(with error: Error) {
        DiagnosticLog.shared.add("failed: \(error.localizedDescription)")
        lastFailure = error
        state = .failed
        // Stay alive for a while: the app reads the log (the only way to see why) and then stops
        // the tunnel itself. Without this the process ends and the log is gone.
        DispatchQueue.global().asyncAfter(deadline: .now() + 60) { [weak self] in
            self?.cancelTunnelWithError(error)
        }
    }
}
