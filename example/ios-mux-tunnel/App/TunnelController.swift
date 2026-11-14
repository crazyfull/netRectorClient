import Foundation
import NetworkExtension

/// Owns the VPN configuration of the system settings, starts and stops the tunnel, and keeps the
/// status (including the number of connected tunnels) up to date for the UI.
/// All methods and the delegate callbacks run on the main thread.
final class TunnelController {
    weak var delegate: TunnelControllerDelegate?

    var configuration: TunnelConfiguration

    private(set) var status = TunnelStatus.stopped {
        didSet { delegate?.tunnelControllerDidUpdate(self) }
    }

    private(set) var errorMessage: String? {
        didSet { delegate?.tunnelControllerDidUpdate(self) }
    }

    /// What the app itself saw (system status changes, errors). Shown in the diagnostics screen,
    /// because it still exists when the extension process is dead.
    private(set) var eventLines: [String] = []

    /// The extension log read when the extension reported a failure (the extension ends afterwards).
    private(set) var failureLog: String?
    private var isCollectingFailureLog = false
    private let eventFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss.SSS"
        return formatter
    }()

    let diagnosticReceiver = DiagnosticReceiver()
    private var userRequestedStop = false
    private var lastReplyWasOk: Bool?

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
        diagnosticReceiver.start()
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
        failureLog = nil
        isCollectingFailureLog = false
        userRequestedStop = false
        diagnosticReceiver.clear()
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
        logEvent("start tapped")

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
                        self.logEvent("startVPNTunnel called")
                    } catch {
                        self.fail(with: error)
                    }
                }
            }
        }
    }

    func logEvent(_ text: String) {
        eventLines.append("\(eventFormatter.string(from: Date())) \(text)")
        if eventLines.count > 100 {
            eventLines.removeFirst(eventLines.count - 100)
        }
    }

    func stop() {
        userRequestedStop = true
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

        if lastLoggedSystemStatus != connection.status {
            lastLoggedSystemStatus = connection.status
            logEvent("system status: \(Self.name(of: connection.status))")
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
            if wasStarting && !userRequestedStop {
                if #available(iOS 16.0, *) {
                    readLastError(from: connection)
                } else {
                    // Older systems do not tell why: most likely the extension crashed or was killed
                    errorMessage = NSLocalizedString("error.tunnelStopped", comment: "")
                }
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
            let reported = response.flatMap { try? TunnelStatus.decoded(from: $0) }
            DispatchQueue.main.async {
                if self?.lastReplyWasOk != (reported != nil) {
                    self?.lastReplyWasOk = reported != nil
                    self?.logEvent(reported != nil ? "extension answers status requests" : "extension does not answer status requests")
                }
                guard let reported = reported else {
                    return
                }
                self?.status = reported
                if reported.state == .failed {
                    self?.collectFailureLog()
                }
            }
        }
    }

    /// Asks the extension for its diagnostic log. `nil` when the extension is not running.
    func fetchDiagnosticLog(completion: @escaping (String?) -> Void) {
        guard let session = manager?.connection as? NETunnelProviderSession,
              let message = try? TunnelProviderRequest.log.encoded() else {
            completion(nil)
            return
        }
        do {
            try session.sendProviderMessage(message) { response in
                let text = response.flatMap { String(data: $0, encoding: .utf8) }
                DispatchQueue.main.async { completion(text) }
            }
        } catch {
            completion(nil)
        }
    }

    /// The extension says it failed: read its log, show the reason and stop the VPN.
    private func collectFailureLog() {
        guard !isCollectingFailureLog else {
            return
        }
        isCollectingFailureLog = true
        fetchDiagnosticLog { [weak self] text in
            guard let self = self else {
                return
            }
            self.failureLog = text
            self.logEvent("the extension reported a failure")
            let reason = text?.components(separatedBy: "\n").last(where: { $0.contains("failed:") })
            self.errorMessage = reason ?? NSLocalizedString("error.tunnelStopped", comment: "")
            self.stop()
        }
    }

    private var lastLoggedSystemStatus: NEVPNStatus?

    private static func name(of status: NEVPNStatus) -> String {
        switch status {
        case .invalid: return "invalid"
        case .disconnected: return "disconnected"
        case .connecting: return "connecting"
        case .connected: return "connected"
        case .reasserting: return "reasserting"
        case .disconnecting: return "disconnecting"
        @unknown default: return "unknown"
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
            self.logEvent("error: \(error.localizedDescription)")
            self.errorMessage = error.localizedDescription
            self.status = TunnelStatus(state: .failed, connectedTunnels: 0, totalTunnels: 0)
        }
    }
}
