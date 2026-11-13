import Foundation

/// Why the Packet Tunnel extension could not start or had to stop.
enum TunnelProviderError: LocalizedError {
    case missingConfiguration
    case noFreePort
    case noTunnelDescriptor
    case tunnelEngineFailed
    case muxClientStopped(code: Int32)

    var errorDescription: String? {
        switch self {
        case .missingConfiguration:
            return "The tunnel configuration is missing."
        case .noFreePort:
            return "No free local port for the SOCKS5 listener."
        case .noTunnelDescriptor:
            return "The tunnel interface was not found."
        case .tunnelEngineFailed:
            return "The tun2socks engine could not start."
        case .muxClientStopped(let code):
            return "The mux client stopped unexpectedly (code \(code))."
        }
    }
}
