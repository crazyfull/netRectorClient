import UIKit

/// A caption with an on/off switch on the right.
final class LabeledSwitch: UIView {
    private let captionLabel = UILabel()
    private let toggle = UISwitch()

    var isOn: Bool {
        get { return toggle.isOn }
        set { toggle.isOn = newValue }
    }

    var isEnabled: Bool {
        get { return toggle.isEnabled }
        set {
            toggle.isEnabled = newValue
            captionLabel.alpha = newValue ? 1 : 0.5
        }
    }

    init(captionKey: String) {
        super.init(frame: .zero)

        captionLabel.text = NSLocalizedString(captionKey, comment: "")
        captionLabel.font = UIFont.systemFont(ofSize: 16)
        captionLabel.numberOfLines = 0

        let row = UIStackView(arrangedSubviews: [captionLabel, toggle])
        row.axis = .horizontal
        row.alignment = .center
        row.spacing = 12
        row.translatesAutoresizingMaskIntoConstraints = false
        addSubview(row)
        NSLayoutConstraint.activate([
            row.topAnchor.constraint(equalTo: topAnchor),
            row.bottomAnchor.constraint(equalTo: bottomAnchor),
            row.leadingAnchor.constraint(equalTo: leadingAnchor),
            row.trailingAnchor.constraint(equalTo: trailingAnchor)
        ])
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
}
