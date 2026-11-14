import Foundation

/// Reads the lines the tun2socks engine appended to its log file since the last call.
final class HevLogReader {
    private var offset: UInt64 = 0
    private var partialLine = ""

    func readNewLines(maxLines: Int = 60) -> [String] {
        guard let handle = FileHandle(forReadingAtPath: HevConfigurationBuilder.logFilePath) else {
            return []
        }
        defer { handle.closeFile() }

        let size = handle.seekToEndOfFile()
        if size < offset {
            offset = 0      // the file was recreated
        }
        guard size > offset else {
            return []
        }
        handle.seek(toFileOffset: offset)
        let data = handle.readData(ofLength: Int(min(size - offset, 16_000)))
        offset += UInt64(data.count)

        var text = partialLine + String(decoding: data, as: UTF8.self)
        var lines = text.components(separatedBy: "\n")
        partialLine = lines.removeLast()
        text = ""
        return Array(lines.filter { !$0.isEmpty }.suffix(maxLines))
    }
}
