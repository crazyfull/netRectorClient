import Foundation

/// Builds the YAML configuration of hev-socks5-tunnel.
enum HevConfigurationBuilder {
    static func makeYaml(socksPort: UInt16, logPath: String) -> String {
        // Small buffers: a Network Extension has a tight memory limit
        return """
        tunnel:
          mtu: \(DefaultTunnelSettings.mtu)
          ipv4: \(DefaultTunnelSettings.tunnelIPv4)
          ipv6: '\(DefaultTunnelSettings.tunnelIPv6)'
        socks5:
          port: \(socksPort)
          address: \(DefaultTunnelSettings.socksAddress)
          udp: 'udp'
        mapdns:
          address: \(DefaultTunnelSettings.dnsAddress)
          port: 53
          network: \(DefaultTunnelSettings.mappedNetwork)
          netmask: \(DefaultTunnelSettings.mappedNetmask)
          cache-size: 5000
        misc:
          task-stack-size: 86016
          tcp-buffer-size: 16384
          max-session-count: 256
          connect-timeout: 10000
          tcp-read-write-timeout: 300000
          log-file: \(logPath)
          log-level: debug

        """
    }

    /// File the engine writes its own log to (read back into the diagnostic log).
    static var logFilePath: String {
        return FileManager.default.temporaryDirectory.appendingPathComponent("hev-socks5-tunnel.log").path
    }

    /// The last part of the engine log, empty when there is none.
    static func readLogTail(maxBytes: Int = 6000) -> String {
        guard let data = FileManager.default.contents(atPath: logFilePath) else {
            return ""
        }
        return String(decoding: data.suffix(maxBytes), as: UTF8.self)
    }

    /// Writes the configuration to a temporary file and returns its path.
    static func writeFile(socksPort: UInt16) throws -> String {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("hev-socks5-tunnel.yml")
        try makeYaml(socksPort: socksPort, logPath: logFilePath).write(to: url, atomically: true, encoding: .utf8)
        return url.path
    }
}
