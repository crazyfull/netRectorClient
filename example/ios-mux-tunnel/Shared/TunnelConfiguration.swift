import Foundation

/// The user's configuration. Saved by the app and handed to the Packet Tunnel extension.
struct TunnelConfiguration: Codable, Equatable {
    var cdnAddress = DefaultTunnelSettings.cdnAddress
    var cdnPort = DefaultTunnelSettings.cdnPort
    var tunnelCount = DefaultTunnelSettings.tunnelCount
    var handshakeHost = DefaultTunnelSettings.handshakeHost
    var handshakePath = DefaultTunnelSettings.handshakePath
    var webSocketKey = DefaultTunnelSettings.webSocketKey
    var expectedAccept = DefaultTunnelSettings.expectedAccept

    /// Throws the first problem found.
    func validate() throws {
        let address = cdnAddress.trimmingCharacters(in: .whitespaces)
        if address.isEmpty || address.contains(" ") {
            throw TunnelConfigurationError.invalidAddress
        }
        if !(1...65535).contains(cdnPort) {
            throw TunnelConfigurationError.invalidPort
        }
        if !(DefaultTunnelSettings.minTunnelCount...DefaultTunnelSettings.maxTunnelCount).contains(tunnelCount) {
            throw TunnelConfigurationError.invalidTunnelCount
        }
        if !handshakePath.hasPrefix("/") {
            throw TunnelConfigurationError.invalidPath
        }
    }

    func encoded() throws -> Data {
        return try JSONEncoder().encode(self)
    }

    static func decoded(from data: Data) throws -> TunnelConfiguration {
        return try JSONDecoder().decode(TunnelConfiguration.self, from: data)
    }
}
