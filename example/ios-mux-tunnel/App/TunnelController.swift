import Combine
import Foundation
import NetworkExtension

/// Owns the VPN configuration of the system settings, starts and stops the tunnel, and keeps the
/// status (including the number of connected tunnels) up to date for the UI.
final class TunnelController: ObservableObject {
    @Published var configuration: TunnelConfiguration
    @Published private(set) var status = TunnelStatus.stopped
    @Published private(set) var errorMessage: String?

    private let store = TunnelConfigurationStore()
    private var manager: NETunnelProviderManager?
    private var statusObserver: NSObjectProtocol?
    private var pollTimer: Timer?
    private let pollInterval: TimeInterval = 1

    /// The settings can be edited only while the tunnel is not running.
    var isEditable: Bool {
        return status.state == .stopped || status.state == .failed
    }

    var isActive: Bool {
        return status.state == .starting || status.state == .running
    }

    init() {
        configuration = store.load()
        loadManager()
    }

    deinit {
        pollTimer?.invalidate()
        if let observer = statusObserver {
            NotificationCenter.default.removeObserver(observer)
        }
    }

    // MARK: - Actions

    func toggle() {
        if isActive {
            stop()
        } else {
            start()
        }
    }

    func start() {
        errorMessage = nil
        do {
            try configuration.validate()
        } catch {
            errorMessage = error.localizedDescription
            return
        }
        store.save(configuration)

        let manager = self.manager ?? NETunnelProviderManager()
        do {
            try apply(configuration, to: manager)
        } catch {
            errorMessage = error.localizedDescription
            return
        }

        status = TunnelStatus(state: .starting, connectedTunnels: 0, totalTunnels: configuration.tunnelCount)

        // Saving shows the system's "Add VPN configuration" prompt the first time
        manager.saveToPreferences { [weak self] error in
            guard let self = self else {
                return
            }
            if let error = error {
                self.fail(with: error)
                return
            }
            // A second load is required after the first save, otherwise starting fails
            manager.loadFromPreferences { error in
                if let error = error {
                    self.fail(with: error)
                    return
                }
                DispatchQueue.main.async {
                    self.attach(manager)
                    do {
                        try manager.connection.startVPNTunnel()
                    } catch {
                        self.fail(with: error)
                    }
                }
            }
        }
    }

    func stop() {
        manager?.connection.stopVPNTunnel()
    }

    // MARK: - System VPN configuration

    private func apply(_ configuration: TunnelConfiguration, to manager: NETunnelProviderManager) throws {
        let protocolConfiguration = NETunnelProviderProtocol()
        protocolConfiguration.providerBundleIdentifier = DefaultTunnelSettings.extensionBundleIdentifier
        protocolConfiguration.serverAddress = configuration.cdnAddress
        protocolConfiguration.providerConfiguration = [TunnelConfigurationKey.configuration: try configuration.encoded()]

        manager.protocolConfiguration = protocolConfiguration
        manager.localizedDescription = "Mux Tunnel"
        manager.isEnabled = true
    }

    private func loadManager() {
        NETunnelProviderManager.loadAllFromPreferences { [weak self] managers, error in
            DispatchQueue.main.async {
                guard let self = self else {
                    return
                }
                if let error = error {
                    self.errorMessage = error.localizedDescription
                    return
                }
                if let existing = managers?.first {
                    self.attach(existing)
                }
            }
        }
    }

    /// Starts following the connection of `manager`.
    private func attach(_ manager: NETunnelProviderManager) {
        self.manager = manager

        if let observer = statusObserver {
            NotificationCenter.default.removeObserver(observer)
        }
        statusObserver = NotificationCenter.default.addObserver(
            forName: .NEVPNStatusDidChange, object: manager.connection, queue: .main) { [weak self] _ in
            self?.systemStatusChanged()
        }

        systemStatusChanged()
        startPolling()
    }

    // MARK: - Status

    private func systemStatusChanged() {
        guard let connection = manager?.connection else {
            return
        }

        switch connection.status {
        case .connected:
            if status.state != .running {
                status = TunnelStatus(state: .running, connectedTunnels: 0, totalTunnels: configuration.tunnelCount)
            }
            requestStatus()
        case .connecting, .reasserting:
            status = TunnelStatus(state: .starting, connectedTunnels: 0, totalTunnels: configuration.tunnelCount)
        case .disconnecting, .disconnected, .invalid:
            let wasStarting = status.state == .starting || status.state == .running
            status = TunnelStatus.stopped
            if wasStarting {
                readLastError(from: connection)
            }
        @unknown default:
            break
        }
    }

    private func startPolling() {
        pollTimer?.invalidate()
        pollTimer = Timer.scheduledTimer(withTimeInterval: pollInterval, repeats: true) { [weak self] _ in
            self?.requestStatus()
        }
    }

    /// Asks the extension how many tunnels are connected.
    private func requestStatus() {
        guard let session = manager?.connection as? NETunnelProviderSession,
              session.status == .connected,
              let message = try? TunnelProviderRequest.status.encoded() else {
            return
        }

        try? session.sendProviderMessage(message) { [weak self] response in
            guard let response = response,
                  let reported = try? TunnelStatus.decoded(from: response) else {
                return
            }
            DispatchQueue.main.async {
                self?.status = reported
            }
        }
    }

    private func readLastError(from connection: NEVPNConnection) {
        guard #available(iOS 16.0, *) else {
            return
        }
        connection.fetchLastDisconnectError { [weak self] error in
            guard let error = error else {
                return
            }
            DispatchQueue.main.async {
                self?.errorMessage = error.localizedDescription
                self?.status = TunnelStatus(state: .failed, connectedTunnels: 0, totalTunnels: 0)
            }
        }
    }

    private func fail(with error: Error) {
        DispatchQueue.main.async {
            self.errorMessage = error.localizedDescription
            self.status = TunnelStatus(state: .failed, connectedTunnels: 0, totalTunnels: 0)
        }
    }
}
