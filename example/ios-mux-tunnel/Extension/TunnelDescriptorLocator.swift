import Foundation
import MuxTunnelCore
import NetworkExtension

/// Finds the file descriptor of the utun interface that belongs to the Packet Tunnel extension.
/// hev-socks5-tunnel reads and writes IP packets on it directly.
enum TunnelDescriptorLocator {
    static func locate(packetFlow: NEPacketTunnelFlow) -> Int32? {
        if let descriptor = packetFlow.value(forKeyPath: "socket.fileDescriptor") as? Int32 {
            return descriptor
        }

        // Fallback: look for the utun control socket among the open descriptors
        let descriptor = muxTunnelCoreFindUtunDescriptor()
        return descriptor >= 0 ? descriptor : nil
    }
}
