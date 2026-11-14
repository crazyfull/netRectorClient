import Foundation

/// Thread-safe log of the extension. Every line is kept in a ring buffer (answer to the app's
/// `.log` request) and sent to the app over the loopback diagnostic channel.
final class DiagnosticLog {
    static let shared = DiagnosticLog()

    private let maxLines = 400
    private var lines: [String] = []
    private let lock = NSLock()
    private var socketDescriptor: Int32 = -1
    private let formatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss.SSS"
        return formatter
    }()

    func add(_ message: String) {
        lock.lock()
        defer { lock.unlock() }

        let line = "\(formatter.string(from: Date())) \(message)"
        lines.append(line)
        if lines.count > maxLines {
            lines.removeFirst(lines.count - maxLines)
        }
        send(line)
    }

    var text: String {
        lock.lock()
        defer { lock.unlock() }
        return lines.joined(separator: "\n")
    }

    /// Must be called with the lock held.
    private func send(_ line: String) {
        if socketDescriptor < 0 {
            socketDescriptor = socket(AF_INET, SOCK_DGRAM, 0)
        }
        guard socketDescriptor >= 0 else {
            return
        }

        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = DiagnosticChannel.port.bigEndian
        address.sin_addr.s_addr = inet_addr(DiagnosticChannel.address)

        let bytes = Array(line.utf8.prefix(1200))
        withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                _ = sendto(socketDescriptor, bytes, bytes.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
    }
}
