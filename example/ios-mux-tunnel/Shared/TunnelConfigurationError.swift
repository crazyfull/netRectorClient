import Foundation

/// A problem in the user's configuration.
enum TunnelConfigurationError: LocalizedError {
    case invalidAddress
    case invalidPort
    case invalidTunnelCount
    case invalidPath

    var errorDescription: String? {
        switch self {
        case .invalidAddress:
            return NSLocalizedString("error.address", comment: "")
        case .invalidPort:
            return NSLocalizedString("error.port", comment: "")
        case .invalidTunnelCount:
            return String(format: NSLocalizedString("error.tunnels", comment: ""),
                          DefaultTunnelSettings.minTunnelCount, DefaultTunnelSettings.maxTunnelCount)
        case .invalidPath:
            return NSLocalizedString("error.path", comment: "")
        }
    }
}
