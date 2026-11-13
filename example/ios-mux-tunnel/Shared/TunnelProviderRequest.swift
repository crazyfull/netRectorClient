import Foundation

/// Messages the app sends to the Packet Tunnel extension (sendProviderMessage).
enum TunnelProviderRequest: String, Codable {
    /// Answered with an encoded TunnelStatus.
    case status

    func encoded() throws -> Data {
        return try JSONEncoder().encode(self)
    }

    static func decoded(from data: Data) throws -> TunnelProviderRequest {
        return try JSONDecoder().decode(TunnelProviderRequest.self, from: data)
    }
}
