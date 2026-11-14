import Foundation

/// Collects the log lines the extension sends over the loopback diagnostic channel.
final class DiagnosticReceiver {
    private let queue = DispatchQueue(label: "diagnostic.receiver")
    private var source: DispatchSourceRead?
    private var lines: [String] = []
    private let lock = NSLock()
    private let maxLines = 500

    /// Called on the main thread when the extension reports its status.
    var onStatus: ((TunnelStatus) -> Void)?

    var text: String {
        lock.lock()
        defer { lock.unlock() }
        return lines.joined(separator: "\n")
    }

    func clear() {
        lock.lock()
        lines.removeAll()
        lock.unlock()
    }

    /// Starts listening. Does nothing when already listening or when the port cannot be bound.
    func start() {
        guard source == nil else {
            return
        }
        let descriptor = socket(AF_INET, SOCK_DGRAM, 0)
        guard descriptor >= 0 else {
            return
        }

        var reuse: Int32 = 1
        setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse, socklen_t(MemoryLayout<Int32>.size))

        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = DiagnosticChannel.port.bigEndian
        address.sin_addr.s_addr = inet_addr(DiagnosticChannel.address)

        let bound = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.bind(descriptor, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bound == 0 else {
            close(descriptor)
            return
        }

        let source = DispatchSource.makeReadSource(fileDescriptor: descriptor, queue: queue)
        source.setEventHandler { [weak self] in
            self?.readPending(from: descriptor)
        }
        source.setCancelHandler {
            close(descriptor)
        }
        source.resume()
        self.source = source
    }

    private func readPending(from descriptor: Int32) {
        var buffer = [UInt8](repeating: 0, count: 2048)
        while true {
            let count = recv(descriptor, &buffer, buffer.count, MSG_DONTWAIT)
            if count <= 0 {
                break
            }
            let message = String(decoding: buffer[0..<count], as: UTF8.self)
            if let status = DiagnosticChannel.status(from: message) {
                DispatchQueue.main.async { [weak self] in
                    self?.onStatus?(status)
                }
            } else {
                append(message)
            }
        }
    }

    private func append(_ line: String) {
        lock.lock()
        lines.append(line)
        if lines.count > maxLines {
            lines.removeFirst(lines.count - maxLines)
        }
        lock.unlock()
    }
}
