import Foundation

/// The extension sends its log lines as UDP datagrams to this loopback port; the app listens.
/// It works even when the extension process dies right after, unlike a request to the extension.
enum DiagnosticChannel {
    static let port: UInt16 = 47321
    static let address = "127.0.0.1"
}
