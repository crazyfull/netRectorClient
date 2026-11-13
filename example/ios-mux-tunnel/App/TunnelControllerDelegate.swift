import Foundation

/// Told, on the main thread, whenever anything the UI shows has changed.
protocol TunnelControllerDelegate: AnyObject {
    func tunnelControllerDidUpdate(_ controller: TunnelController)
}
