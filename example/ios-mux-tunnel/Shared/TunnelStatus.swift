import Foundation

/// What the status bar shows. The extension reports it; the app maps the system VPN state when the
/// extension cannot answer.
struct TunnelStatus: Codable, Equatable {
    var state: TunnelConnectionState
    /// Tunnels that finished the handshake and are connected right now.
    var connectedTunnels: Int
    /// Tunnels the configuration asks for.
    var totalTunnels: Int

    static let stopped = TunnelStatus(state: .stopped, connectedTunnels: 0, totalTunnels: 0)

    func encoded() throws -> Data {
        return try JSONEncoder().encode(self)
    }

    static func decoded(from data: Data) throws -> TunnelStatus {
        return try JSONDecoder().decode(TunnelStatus.self, from: data)
    }
}
