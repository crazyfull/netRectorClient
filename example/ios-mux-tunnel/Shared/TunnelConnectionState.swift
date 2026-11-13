import Foundation

/// Where the tunnel is in its life cycle.
enum TunnelConnectionState: String, Codable {
    case stopped
    case starting
    case running
    case failed
}
