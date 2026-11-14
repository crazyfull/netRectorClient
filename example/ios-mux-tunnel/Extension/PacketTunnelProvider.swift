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

    // The state is written from the provider queue and from the engine threads
    private let stateLock = NSLock()
    private var storedState = TunnelConnectionState.stopped
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
        muxTunnelCoreSetLogFunction { level, message in
            if let message = message {
                DiagnosticLog.shared.add("core[\(level)] \(String(cString: message))")
            }
        }
        DiagnosticLog.shared.add("startTunnel")

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
            self.configuration = configuration

            guard let socksPort = LoopbackPortFinder.findFreePort() else {
                throw TunnelProviderError.noFreePort
            }
            let configurationPath = try HevConfigurationBuilder.writeFile(socksPort: socksPort)
            DiagnosticLog.shared.add("socks5 listener 127.0.0.1:\(socksPort)")

            muxCore.start(configuration: configuration, socksPort: socksPort) { [weak self] code in
                DiagnosticLog.shared.add("mux core stopped, code \(code)")
                self?.fail(with: TunnelProviderError.muxClientStopped(code: code))
            }

            setTunnelNetworkSettings(TunnelNetworkSettingsFactory.makeSettings()) { [weak self] error in
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
                DiagnosticLog.shared.add("network settings applied")
                self.startTunnelEngine(configurationPath: configurationPath, completionHandler: completionHandler)
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
        completionHandler(nil)
    }

    private func currentStatus() -> TunnelStatus {
        return TunnelStatus(state: state,
                            connectedTunnels: state == .running ? muxCore.connectedTunnels : 0,
                            totalTunnels: configuration?.tunnelCount ?? 0)
    }

    /// The log plus the live numbers: packets moved through the utun interface tell where it stops.
    private func diagnosticText() -> String {
        var txPackets = 0, txBytes = 0, rxPackets = 0, rxBytes = 0
        hev_socks5_tunnel_stats(&txPackets, &txBytes, &rxPackets, &rxBytes)
        let header = "state \(state), tunnels \(muxCore.connectedTunnels)/\(configuration?.tunnelCount ?? 0)\n"
            + "utun -> engine: \(txPackets) packets, \(txBytes) bytes\n"
            + "engine -> utun: \(rxPackets) packets, \(rxBytes) bytes\n"
            + "memory: \(Self.memoryFootprintMegabytes()) MB\n---\n"
        return header + DiagnosticLog.shared.text
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

    private func stopEngines() {
        tunnelEngine.stop()
        muxCore.stop()
    }

    /// Called when one of the engines ends by itself: the VPN cannot work without both.
    private func fail(with error: Error) {
        state = .failed
        cancelTunnelWithError(error)
    }
}
