import UIKit

/// The only screen: status bar, Start / Stop button and the settings.
final class MainViewController: UIViewController, TunnelControllerDelegate {
    private let controller = TunnelController()

    private let statusBar = StatusBarView()
    private let scrollView = UIScrollView()
    private let contentStack = UIStackView()
    private let startButton = UIButton(type: .system)
    private let errorLabel = UILabel()

    private let addressField = LabeledTextField(captionKey: "field.address", keyboardType: .URL)
    private let portField = LabeledTextField(captionKey: "field.port", keyboardType: .numberPad)
    private let tunnelCountView = TunnelCountView()
    private let handshakeHostField = LabeledTextField(captionKey: "field.handshakeHost")
    private let handshakePathField = LabeledTextField(captionKey: "field.handshakePath")
    private let webSocketKeyField = LabeledTextField(captionKey: "field.webSocketKey")
    private let expectedAcceptField = LabeledTextField(captionKey: "field.expectedAccept")

    // MARK: - Life cycle

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .white

        buildLayout()
        show(controller.configuration)

        controller.delegate = self
        tunnelControllerDidUpdate(controller)

        let center = NotificationCenter.default
        center.addObserver(self, selector: #selector(keyboardWillChange(_:)),
                           name: UIResponder.keyboardWillChangeFrameNotification, object: nil)
        center.addObserver(self, selector: #selector(keyboardWillHide(_:)),
                           name: UIResponder.keyboardWillHideNotification, object: nil)
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    // MARK: - TunnelControllerDelegate

    func tunnelControllerDidUpdate(_ controller: TunnelController) {
        statusBar.update(status: controller.status)

        let titleKey = controller.isActive ? "button.stop" : "button.start"
        startButton.setTitle(NSLocalizedString(titleKey, comment: ""), for: .normal)
        startButton.backgroundColor = controller.isActive ? AppColor.error : AppColor.accent

        errorLabel.text = controller.errorMessage
        errorLabel.isHidden = controller.errorMessage == nil

        let editable = controller.isEditable
        [addressField, portField, handshakeHostField, handshakePathField,
         webSocketKeyField, expectedAcceptField].forEach { $0.isEnabled = editable }
        tunnelCountView.isEnabled = editable
    }

    // MARK: - Actions

    @objc private func startStopTapped() {
        view.endEditing(true)
        if !controller.isActive {
            controller.configuration = readConfiguration()
        }
        controller.toggle()
    }

    private func show(_ configuration: TunnelConfiguration) {
        addressField.text = configuration.cdnAddress
        portField.text = String(configuration.cdnPort)
        tunnelCountView.value = configuration.tunnelCount
        handshakeHostField.text = configuration.handshakeHost
        handshakePathField.text = configuration.handshakePath
        webSocketKeyField.text = configuration.webSocketKey
        expectedAcceptField.text = configuration.expectedAccept
    }

    private func readConfiguration() -> TunnelConfiguration {
        var configuration = TunnelConfiguration()
        configuration.cdnAddress = addressField.text.trimmingCharacters(in: .whitespaces)
        configuration.cdnPort = Int(portField.text.trimmingCharacters(in: .whitespaces)) ?? 0
        configuration.tunnelCount = tunnelCountView.value
        configuration.handshakeHost = handshakeHostField.text.trimmingCharacters(in: .whitespaces)
        configuration.handshakePath = handshakePathField.text.trimmingCharacters(in: .whitespaces)
        configuration.webSocketKey = webSocketKeyField.text.trimmingCharacters(in: .whitespaces)
        configuration.expectedAccept = expectedAcceptField.text.trimmingCharacters(in: .whitespaces)
        return configuration
    }

    // MARK: - Layout

    private func buildLayout() {
        startButton.setTitleColor(.white, for: .normal)
        startButton.titleLabel?.font = UIFont.systemFont(ofSize: 17, weight: .semibold)
        startButton.layer.cornerRadius = 10
        startButton.addTarget(self, action: #selector(startStopTapped), for: .touchUpInside)
        startButton.heightAnchor.constraint(equalToConstant: 48).isActive = true

        errorLabel.font = UIFont.systemFont(ofSize: 14)
        errorLabel.textColor = AppColor.error
        errorLabel.numberOfLines = 0
        errorLabel.isHidden = true

        let noteLabel = UILabel()
        noteLabel.text = NSLocalizedString("note.tcpOnly", comment: "")
        noteLabel.font = UIFont.systemFont(ofSize: 13)
        noteLabel.textColor = AppColor.secondaryText
        noteLabel.numberOfLines = 0

        contentStack.axis = .vertical
        contentStack.spacing = 14
        [startButton,
         errorLabel,
         makeSectionTitle("section.server"),
         addressField,
         portField,
         tunnelCountView,
         makeSectionTitle("section.handshake"),
         handshakeHostField,
         handshakePathField,
         webSocketKeyField,
         expectedAcceptField,
         noteLabel].forEach { contentStack.addArrangedSubview($0) }

        scrollView.keyboardDismissMode = .interactive
        scrollView.alwaysBounceVertical = true

        [statusBar, scrollView, contentStack].forEach { $0.translatesAutoresizingMaskIntoConstraints = false }
        view.addSubview(scrollView)
        view.addSubview(statusBar)
        scrollView.addSubview(contentStack)

        NSLayoutConstraint.activate([
            statusBar.topAnchor.constraint(equalTo: view.topAnchor),
            statusBar.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            statusBar.trailingAnchor.constraint(equalTo: view.trailingAnchor),

            scrollView.topAnchor.constraint(equalTo: statusBar.bottomAnchor),
            scrollView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            scrollView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            scrollView.bottomAnchor.constraint(equalTo: view.bottomAnchor),

            contentStack.topAnchor.constraint(equalTo: scrollView.contentLayoutGuide.topAnchor, constant: 16),
            contentStack.bottomAnchor.constraint(equalTo: scrollView.contentLayoutGuide.bottomAnchor, constant: -24),
            contentStack.leadingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.leadingAnchor, constant: 16),
            contentStack.trailingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.trailingAnchor, constant: -16),
            contentStack.widthAnchor.constraint(equalTo: scrollView.frameLayoutGuide.widthAnchor, constant: -32)
        ])
    }

    private func makeSectionTitle(_ key: String) -> UILabel {
        let label = UILabel()
        label.text = NSLocalizedString(key, comment: "")
        label.font = UIFont.systemFont(ofSize: 18, weight: .semibold)
        return label
    }

    // MARK: - Keyboard

    @objc private func keyboardWillChange(_ notification: Notification) {
        guard let frame = (notification.userInfo?[UIResponder.keyboardFrameEndUserInfoKey] as? NSValue)?.cgRectValue else {
            return
        }
        let overlap = max(0, view.convert(view.bounds, to: nil).maxY - frame.minY)
        scrollView.contentInset.bottom = overlap
        scrollView.scrollIndicatorInsets.bottom = overlap
    }

    @objc private func keyboardWillHide(_ notification: Notification) {
        scrollView.contentInset.bottom = 0
        scrollView.scrollIndicatorInsets.bottom = 0
    }
}
