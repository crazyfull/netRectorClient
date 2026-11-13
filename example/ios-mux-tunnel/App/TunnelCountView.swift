import UIKit

/// "Number of tunnels: N" with a stepper.
final class TunnelCountView: UIView {
    private let titleLabel = UILabel()
    private let stepper = UIStepper()

    var value: Int {
        get { return Int(stepper.value) }
        set {
            stepper.value = Double(newValue)
            updateTitle()
        }
    }

    var isEnabled: Bool {
        get { return stepper.isEnabled }
        set {
            stepper.isEnabled = newValue
            titleLabel.alpha = newValue ? 1 : 0.5
        }
    }

    override init(frame: CGRect) {
        super.init(frame: frame)

        stepper.minimumValue = Double(DefaultTunnelSettings.minTunnelCount)
        stepper.maximumValue = Double(DefaultTunnelSettings.maxTunnelCount)
        stepper.stepValue = 1
        stepper.addTarget(self, action: #selector(valueChanged), for: .valueChanged)

        titleLabel.font = UIFont.systemFont(ofSize: 16)
        updateTitle()

        [titleLabel, stepper].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            addSubview($0)
        }
        NSLayoutConstraint.activate([
            titleLabel.leadingAnchor.constraint(equalTo: leadingAnchor),
            titleLabel.centerYAnchor.constraint(equalTo: centerYAnchor),
            stepper.trailingAnchor.constraint(equalTo: trailingAnchor),
            stepper.topAnchor.constraint(equalTo: topAnchor, constant: 4),
            stepper.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -4),
            stepper.leadingAnchor.constraint(greaterThanOrEqualTo: titleLabel.trailingAnchor, constant: 8)
        ])
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    @objc private func valueChanged() {
        updateTitle()
    }

    private func updateTitle() {
        titleLabel.text = String(format: NSLocalizedString("field.tunnels", comment: ""), value)
    }
}
