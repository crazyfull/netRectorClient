import Foundation

/// Where the local SOCKS5 listener is, as seen by the tun2socks engine.
struct LoopbackEndpoint {
    let port: UInt16
    /// "::1" when the IPv6 loopback is usable on the same port, otherwise "127.0.0.1".
    let engineAddress: String
}

/// Finds a free TCP port on the loopback interface.
enum LoopbackPortFinder {
    static func findEndpoint() -> LoopbackEndpoint? {
        guard let port = findFreePort() else {
            return nil
        }
        // The mux client listens on both loopbacks (same port). The engine connects over IPv6 when it can.
        let address = canBindIPv6Loopback(port: port) ? DefaultTunnelSettings.socksAddressIPv6 : DefaultTunnelSettings.socksAddress
        return LoopbackEndpoint(port: port, engineAddress: address)
    }

    static func findFreePort() -> UInt16? {
        let descriptor = socket(AF_INET, SOCK_STREAM, 0)
        guard descriptor >= 0 else {
            return nil
        }
        defer { close(descriptor) }

        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = 0
        address.sin_addr.s_addr = inet_addr(DefaultTunnelSettings.socksAddress)

        let addressSize = socklen_t(MemoryLayout<sockaddr_in>.size)
        let bindResult = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(descriptor, $0, addressSize) }
        }
        guard bindResult == 0 else {
            return nil
        }

        var length = addressSize
        let nameResult = withUnsafeMutablePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(descriptor, $0, &length) }
        }
        guard nameResult == 0 else {
            return nil
        }
        return UInt16(bigEndian: address.sin_port)
    }

    /// True when a TCP socket can be bound to ::1 on this port.
    static func canBindIPv6Loopback(port: UInt16) -> Bool {
        let descriptor = socket(AF_INET6, SOCK_STREAM, 0)
        guard descriptor >= 0 else {
            return false
        }
        defer { close(descriptor) }

        var address = sockaddr_in6()
        address.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size)
        address.sin6_family = sa_family_t(AF_INET6)
        address.sin6_port = port.bigEndian
        address.sin6_addr = in6addr_loopback

        let result = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(descriptor, $0, socklen_t(MemoryLayout<sockaddr_in6>.size))
            }
        }
        return result == 0
    }
}
