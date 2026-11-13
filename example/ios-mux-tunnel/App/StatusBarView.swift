import SwiftUI

/// The bar at the top of the screen: state of the tunnel and how many tunnels are really connected.
struct StatusBarView: View {
    let status: TunnelStatus

    var body: some View {
        HStack(spacing: 10) {
            Circle()
                .fill(indicatorColor)
                .frame(width: 12, height: 12)

            Text(stateText)
                .font(.subheadline.weight(.semibold))

            Spacer()

            if status.state == .running || status.state == .starting {
                Text(String(format: NSLocalizedString("statusBar.tunnels", comment: ""),
                            status.connectedTunnels, status.totalTunnels))
                    .font(.subheadline.monospacedDigit())
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
        .frame(maxWidth: .infinity)
        .background(.regularMaterial)
    }

    private var stateText: String {
        switch status.state {
        case .stopped:
            return NSLocalizedString("status.stopped", comment: "")
        case .starting:
            return NSLocalizedString("status.starting", comment: "")
        case .running:
            return status.connectedTunnels > 0
                ? NSLocalizedString("status.connected", comment: "")
                : NSLocalizedString("status.noTunnel", comment: "")
        case .failed:
            return NSLocalizedString("status.failed", comment: "")
        }
    }

    /// Green: all tunnels are up. Orange: starting or only some tunnels are up. Red: no tunnel is up
    /// or the start failed. Gray: stopped.
    private var indicatorColor: Color {
        switch status.state {
        case .stopped:
            return .gray
        case .starting:
            return .orange
        case .running:
            if status.connectedTunnels == 0 {
                return .red
            }
            return status.connectedTunnels >= status.totalTunnels ? .green : .orange
        case .failed:
            return .red
        }
    }
}
