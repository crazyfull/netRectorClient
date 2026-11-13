import UIKit

/// A small caption above a text field.
final class LabeledTextField: UIView {
    private let captionLabel = UILabel()
    private let textField = UITextField()

    var text: String {
        get { return textField.text ?? "" }
        set { textField.text = newValue }
    }

    var isEnabled: Bool {
        get { return textField.isEnabled }
        set {
            textField.isEnabled = newValue
            textField.alpha = newValue ? 1 : 0.5
        }
    }

    init(captionKey: String, keyboardType: UIKeyboardType = .default) {
        super.init(frame: .zero)

        captionLabel.text = NSLocalizedString(captionKey, comment: "")
        captionLabel.font = UIFont.systemFont(ofSize: 13)
        captionLabel.textColor = AppColor.secondaryText
        captionLabel.numberOfLines = 0

        textField.borderStyle = .roundedRect
        textField.keyboardType = keyboardType
        textField.autocapitalizationType = .none
        textField.autocorrectionType = .no
        textField.spellCheckingType = .no
        textField.clearButtonMode = .whileEditing
        textField.returnKeyType = .done
        textField.addTarget(self, action: #selector(returnPressed), for: .editingDidEndOnExit)

        [captionLabel, textField].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            addSubview($0)
        }
        NSLayoutConstraint.activate([
            captionLabel.topAnchor.constraint(equalTo: topAnchor),
            captionLabel.leadingAnchor.constraint(equalTo: leadingAnchor),
            captionLabel.trailingAnchor.constraint(equalTo: trailingAnchor),
            textField.topAnchor.constraint(equalTo: captionLabel.bottomAnchor, constant: 4),
            textField.leadingAnchor.constraint(equalTo: leadingAnchor),
            textField.trailingAnchor.constraint(equalTo: trailingAnchor),
            textField.bottomAnchor.constraint(equalTo: bottomAnchor),
            textField.heightAnchor.constraint(greaterThanOrEqualToConstant: 36)
        ])
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    @objc private func returnPressed() {
        textField.resignFirstResponder()
    }
}
