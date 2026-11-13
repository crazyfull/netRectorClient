import UIKit

/// The bar at the top of the screen: state of the tunnel and how many tunnels are really connected.
final class StatusBarView: UIView {
    private let indicatorView = UIView()
    private let stateLabel = UILabel()
    private let tunnelsLabel = UILabel()
    private let separatorView = UIView()

    override init(frame: CGRect) {
        super.init(frame: frame)
        setUp()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    func update(status: TunnelStatus) {
        indicatorView.backgroundColor = indicatorColor(for: status)
        stateLabel.text = stateText(for: status)

        let showsTunnels = status.state == .running || status.state == .starting
        tunnelsLabel.isHidden = !showsTunnels
        tunnelsLabel.text = String(format: NSLocalizedString("statusBar.tunnels", comment: ""),
                                   status.connectedTunnels, status.totalTunnels)
    }

    // MARK: - Layout

    private func setUp() {
        backgroundColor = AppColor.barBackground

        indicatorView.layer.cornerRadius = 6
        indicatorView.backgroundColor = AppColor.inactive

        stateLabel.font = UIFont.systemFont(ofSize: 15, weight: .semibold)
        stateLabel.text = NSLocalizedString("status.stopped", comment: "")

        tunnelsLabel.font = UIFont.systemFont(ofSize: 15, weight: .regular)
        tunnelsLabel.textColor = AppColor.secondaryText
        tunnelsLabel.isHidden = true
        tunnelsLabel.setContentHuggingPriority(.required, for: .horizontal)

        separatorView.backgroundColor = AppColor.separator

        [indicatorView, stateLabel, tunnelsLabel, separatorView].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            addSubview($0)
        }

        let content = safeAreaLayoutGuide
        NSLayoutConstraint.activate([
            indicatorView.widthAnchor.constraint(equalToConstant: 12),
            indicatorView.heightAnchor.constraint(equalToConstant: 12),
            indicatorView.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 16),
            indicatorView.centerYAnchor.constraint(equalTo: stateLabel.centerYAnchor),

            stateLabel.leadingAnchor.constraint(equalTo: indicatorView.trailingAnchor, constant: 10),
            stateLabel.topAnchor.constraint(equalTo: content.topAnchor, constant: 12),
            stateLabel.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -12),

            tunnelsLabel.leadingAnchor.constraint(greaterThanOrEqualTo: stateLabel.trailingAnchor, constant: 8),
            tunnelsLabel.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -16),
            tunnelsLabel.centerYAnchor.constraint(equalTo: stateLabel.centerYAnchor),

            separatorView.leadingAnchor.constraint(equalTo: leadingAnchor),
            separatorView.trailingAnchor.constraint(equalTo: trailingAnchor),
            separatorView.bottomAnchor.constraint(equalTo: bottomAnchor),
            separatorView.heightAnchor.constraint(equalToConstant: 1 / UIScreen.main.scale)
        ])
    }

    // MARK: - Content

    private func stateText(for status: TunnelStatus) -> String {
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
    private func indicatorColor(for status: TunnelStatus) -> UIColor {
        switch status.state {
        case .stopped:
            return AppColor.inactive
        case .starting:
            return AppColor.warning
        case .running:
            if status.connectedTunnels == 0 {
                return AppColor.error
            }
            return status.connectedTunnels >= status.totalTunnels ? AppColor.connected : AppColor.warning
        case .failed:
            return AppColor.error
        }
    }
}
