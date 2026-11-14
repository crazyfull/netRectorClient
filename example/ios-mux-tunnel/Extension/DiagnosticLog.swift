import Foundation

/// Thread-safe ring buffer with the last log lines of the extension. The app reads it through
/// the `.log` provider message (a Network Extension has no console the user can look at).
final class DiagnosticLog {
    static let shared = DiagnosticLog()

    private let maxLines = 300
    private var lines: [String] = []
    private let lock = NSLock()
    private let formatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss.SSS"
        return formatter
    }()

    func add(_ message: String) {
        lock.lock()
        lines.append("\(formatter.string(from: Date())) \(message)")
        if lines.count > maxLines {
            lines.removeFirst(lines.count - maxLines)
        }
        lock.unlock()
    }

    var text: String {
        lock.lock()
        defer { lock.unlock() }
        return lines.joined(separator: "\n")
    }
}
