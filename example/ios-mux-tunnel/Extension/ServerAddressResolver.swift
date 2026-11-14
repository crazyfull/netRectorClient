import Foundation

/// Turns the server host name into an IP address *before* the VPN routes and DNS settings are
/// applied. Afterwards the system resolver of the extension would ask the tunnel's own DNS, which
/// answers with mapped addresses that lead back into the tunnel (a loop).
enum ServerAddressResolver {
    /// Returns `host` itself when it already is an IP address. IPv4 is preferred.
    static func resolve(_ host: String) -> String? {
        var hints = addrinfo()
        hints.ai_family = AF_UNSPEC
        hints.ai_socktype = SOCK_STREAM

        var result: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo(host, nil, &hints, &result) == 0, let first = result else {
            return nil
        }
        defer { freeaddrinfo(result) }

        var fallback: String?
        var current: UnsafeMutablePointer<addrinfo>? = first
        while let entry = current {
            if let text = numericAddress(of: entry.pointee) {
                if entry.pointee.ai_family == AF_INET {
                    return text
                }
                fallback = fallback ?? text
            }
            current = entry.pointee.ai_next
        }
        return fallback
    }

    private static func numericAddress(of entry: addrinfo) -> String? {
        var buffer = [CChar](repeating: 0, count: Int(NI_MAXHOST))
        let code = getnameinfo(entry.ai_addr, entry.ai_addrlen, &buffer, socklen_t(buffer.count),
                               nil, 0, NI_NUMERICHOST)
        return code == 0 ? String(cString: buffer) : nil
    }
}
