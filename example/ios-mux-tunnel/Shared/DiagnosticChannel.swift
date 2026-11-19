import Foundation

/// The extension sends its log lines as UDP datagrams to this loopback port; the app listens.
/// It works even when the extension process dies right after, unlike a request to the extension.
enum DiagnosticChannel {
    static let port: UInt16 = 47321
    static let address = "127.0.0.1"

    /// Datagrams that start with this carry the tunnel status instead of a log line.
    private static let statusPrefix = "#status "

    static func statusMessage(for status: TunnelStatus) -> String {
        // the optional 4th number is the UDP tunnel: 0 = off, 1 = idle, 2 = connected
        let udp = status.udpTunnelUp.map { $0 ? 2 : 1 } ?? 0
        return "\(statusPrefix)\(status.state.rawValue) \(status.connectedTunnels) \(status.totalTunnels) \(udp)"
    }

    /// `nil` when `message` is a normal log line.
    static func status(from message: String) -> TunnelStatus? {
        guard message.hasPrefix(statusPrefix) else {
            return nil
        }
        let parts = message.dropFirst(statusPrefix.count).split(separator: " ")
        guard parts.count == 3 || parts.count == 4,
              let state = TunnelConnectionState(rawValue: String(parts[0])),
              let connected = Int(parts[1]),
              let total = Int(parts[2]) else {
            return nil
        }
        var udp: Bool?
        if parts.count == 4, let code = Int(parts[3]), code > 0 {
            udp = code == 2
        }
        return TunnelStatus(state: state, connectedTunnels: connected, totalTunnels: total, udpTunnelUp: udp)
    }
}
