import Foundation

/// Keeps the user's configuration between launches (UserDefaults).
final class TunnelConfigurationStore {
    private let defaults: UserDefaults
    private let key = "tunnelConfiguration"

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    func load() -> TunnelConfiguration {
        guard let data = defaults.data(forKey: key),
              let configuration = try? TunnelConfiguration.decoded(from: data) else {
            return TunnelConfiguration()
        }
        return configuration
    }

    func save(_ configuration: TunnelConfiguration) {
        if let data = try? configuration.encoded() {
            defaults.set(data, forKey: key)
        }
    }
}
