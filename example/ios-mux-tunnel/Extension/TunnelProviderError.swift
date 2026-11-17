import Foundation
import MuxTunnelCore

/// Why the Packet Tunnel extension could not start or had to stop.
enum TunnelProviderError: LocalizedError {
    case missingConfiguration
    case noFreePort
    case noTunnelDescriptor
    case tunnelEngineFailed
    case cannotResolveServer(host: String)
    case muxClientStopped(code: Int32)
    case serverNotAnswering

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
        case .cannotResolveServer(let host):
            return "Cannot resolve the server address \(host)."
        case .muxClientStopped(let code):
            if code == Int32(muxTunnelCoreResultTlsUnavailable) {
                return "TLS is not available in this build."
            }
            return "The mux client stopped unexpectedly (code \(code))."
        case .serverNotAnswering:
            return "The server did not answer in time."
        }
    }
}
