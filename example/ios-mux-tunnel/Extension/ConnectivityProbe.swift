import Foundation

/// One-off TCP connect to the server, reported to the diagnostic log. The local address of the
/// socket shows the route: the tunnel's own address means the connection looped into the VPN.
enum ConnectivityProbe {
    static func run(label: String, address: String, port: UInt16) {
        DispatchQueue.global().async {
            DiagnosticLog.shared.add("probe[\(label)] \(address):\(port) \(connect(address: address, port: port))")
        }
    }

    private static func connect(address: String, port: UInt16) -> String {
        var target = sockaddr_in()
        target.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        target.sin_family = sa_family_t(AF_INET)
        target.sin_port = port.bigEndian
        guard inet_pton(AF_INET, address, &target.sin_addr) == 1 else {
            return "not an IPv4 address"
        }

        let descriptor = socket(AF_INET, SOCK_STREAM, 0)
        guard descriptor >= 0 else {
            return "socket() failed"
        }
        defer { close(descriptor) }
        _ = fcntl(descriptor, F_SETFL, fcntl(descriptor, F_GETFL) | O_NONBLOCK)

        let started = Date()
        let result = withUnsafePointer(to: &target) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(descriptor, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        if result != 0 && errno != EINPROGRESS {
            return "connect error: \(String(cString: strerror(errno)))"
        }

        var pollDescriptor = pollfd(fd: descriptor, events: Int16(POLLOUT), revents: 0)
        let ready = poll(&pollDescriptor, 1, 5000)
        let elapsed = Int(Date().timeIntervalSince(started) * 1000)
        if ready <= 0 {
            return "timeout after \(elapsed) ms"
        }

        var socketError: Int32 = 0
        var length = socklen_t(MemoryLayout<Int32>.size)
        getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socketError, &length)
        if socketError != 0 {
            return "failed after \(elapsed) ms: \(String(cString: strerror(socketError)))"
        }
        return "connected in \(elapsed) ms, local address \(localAddress(of: descriptor))"
    }

    private static func localAddress(of descriptor: Int32) -> String {
        var local = sockaddr_in()
        var length = socklen_t(MemoryLayout<sockaddr_in>.size)
        let result = withUnsafeMutablePointer(to: &local) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                getsockname(descriptor, $0, &length)
            }
        }
        guard result == 0 else {
            return "unknown"
        }
        var buffer = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
        inet_ntop(AF_INET, &local.sin_addr, &buffer, socklen_t(buffer.count))
        return String(cString: buffer)
    }
}
