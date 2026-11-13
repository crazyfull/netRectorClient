import Foundation
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

        do {
            let configuration = try loadConfiguration()
            try configuration.validate()
            self.configuration = configuration

            guard let socksPort = LoopbackPortFinder.findFreePort() else {
                throw TunnelProviderError.noFreePort
            }
            let configurationPath = try HevConfigurationBuilder.writeFile(socksPort: socksPort)

            muxCore.start(configuration: configuration, socksPort: socksPort) { [weak self] code in
                self?.fail(with: TunnelProviderError.muxClientStopped(code: code))
            }

            setTunnelNetworkSettings(TunnelNetworkSettingsFactory.makeSettings()) { [weak self] error in
                guard let self = self else {
                    return
                }
                if let error = error {
                    self.stopEngines()
                    self.state = .failed
                    completionHandler(error)
                    return
                }
                self.startTunnelEngine(configurationPath: configurationPath, completionHandler: completionHandler)
            }
        } catch {
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
            stopEngines()
            state = .failed
            completionHandler(TunnelProviderError.noTunnelDescriptor)
            return
        }

        tunnelEngine.start(configurationPath: configurationPath, tunnelDescriptor: descriptor) { [weak self] _ in
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
