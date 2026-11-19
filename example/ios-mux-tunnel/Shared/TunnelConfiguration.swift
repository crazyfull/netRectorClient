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
    /// Wrap the tunnel connections in TLS. The server name (SNI) is `handshakeHost`.
    var useTLS = DefaultTunnelSettings.useTLS
    /// TLS record-layer "SNI bypass" (only with TLS).
    var sniBypass = DefaultTunnelSettings.sniBypass
    /// UDP over TCP (games, calls) on a separate tunnel that connects on demand and closes when idle.
    var useUDP = DefaultTunnelSettings.useUDP

    init() {}

    // A configuration saved by an older version has no `useTLS`: decode with defaults for missing keys.
    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        cdnAddress = try values.decodeIfPresent(String.self, forKey: .cdnAddress) ?? cdnAddress
        cdnPort = try values.decodeIfPresent(Int.self, forKey: .cdnPort) ?? cdnPort
        tunnelCount = try values.decodeIfPresent(Int.self, forKey: .tunnelCount) ?? tunnelCount
        handshakeHost = try values.decodeIfPresent(String.self, forKey: .handshakeHost) ?? handshakeHost
        handshakePath = try values.decodeIfPresent(String.self, forKey: .handshakePath) ?? handshakePath
        webSocketKey = try values.decodeIfPresent(String.self, forKey: .webSocketKey) ?? webSocketKey
        expectedAccept = try values.decodeIfPresent(String.self, forKey: .expectedAccept) ?? expectedAccept
        useTLS = try values.decodeIfPresent(Bool.self, forKey: .useTLS) ?? useTLS
        sniBypass = try values.decodeIfPresent(Bool.self, forKey: .sniBypass) ?? sniBypass
        useUDP = try values.decodeIfPresent(Bool.self, forKey: .useUDP) ?? useUDP
    }

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
