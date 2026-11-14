import Foundation
import NetworkExtension

/// Builds the system-level settings of the VPN: all IPv4 and IPv6 traffic and all DNS queries go
/// into the tunnel, except the connections to the server itself (they would loop into the tunnel).
enum TunnelNetworkSettingsFactory {
    /// `serverAddress` is the IP address the mux tunnels connect to.
    static func makeSettings(excludingServerAddress serverAddress: String) -> NEPacketTunnelNetworkSettings {
        let isIPv6 = serverAddress.contains(":")

        let settings = NEPacketTunnelNetworkSettings(tunnelRemoteAddress: DefaultTunnelSettings.tunnelRemoteAddress)

        let ipv4 = NEIPv4Settings(addresses: [DefaultTunnelSettings.tunnelIPv4],
                                  subnetMasks: [DefaultTunnelSettings.tunnelIPv4Mask])
        ipv4.includedRoutes = [NEIPv4Route.default()]
        if !isIPv6 {
            ipv4.excludedRoutes = [NEIPv4Route(destinationAddress: serverAddress, subnetMask: "255.255.255.255")]
        }
        settings.ipv4Settings = ipv4

        let ipv6 = NEIPv6Settings(addresses: [DefaultTunnelSettings.tunnelIPv6],
                                  networkPrefixLengths: [NSNumber(value: DefaultTunnelSettings.tunnelIPv6PrefixLength)])
        ipv6.includedRoutes = [NEIPv6Route.default()]
        if isIPv6 {
            ipv6.excludedRoutes = [NEIPv6Route(destinationAddress: serverAddress, networkPrefixLength: 128)]
        }
        settings.ipv6Settings = ipv6

        // The engine answers DNS itself (mapped DNS), so apps connect by domain name through the tunnel
        let dns = NEDNSSettings(servers: [DefaultTunnelSettings.dnsAddress])
        dns.matchDomains = [""]
        settings.dnsSettings = dns

        settings.mtu = NSNumber(value: DefaultTunnelSettings.mtu)
        return settings
    }
}
