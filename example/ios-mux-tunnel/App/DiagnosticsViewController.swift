import UIKit

/// Shows the diagnostic log of the Packet Tunnel extension (what it did, packet counters).
final class DiagnosticsViewController: UIViewController {
    private let controller: TunnelController
    private let textView = UITextView()
    private var refreshTimer: Timer?

    init(controller: TunnelController) {
        self.controller = controller
        super.init(nibName: nil, bundle: nil)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .white
        title = NSLocalizedString("diagnostics.title", comment: "")

        navigationItem.leftBarButtonItem = UIBarButtonItem(
            title: NSLocalizedString("diagnostics.close", comment: ""),
            style: .plain, target: self, action: #selector(closeTapped))
        navigationItem.rightBarButtonItems = [
            UIBarButtonItem(title: NSLocalizedString("diagnostics.refresh", comment: ""),
                            style: .plain, target: self, action: #selector(refresh)),
            UIBarButtonItem(title: NSLocalizedString("diagnostics.copy", comment: ""),
                            style: .plain, target: self, action: #selector(copyTapped))
        ]

        textView.isEditable = false
        textView.font = UIFont.monospacedDigitSystemFont(ofSize: 11, weight: .regular)
        textView.textAlignment = .left
        textView.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(textView)
        NSLayoutConstraint.activate([
            textView.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            textView.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            textView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            textView.trailingAnchor.constraint(equalTo: view.trailingAnchor)
        ])

        refresh()
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        refreshTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            self?.refresh()
        }
    }

    override func viewWillDisappear(_ animated: Bool) {
        super.viewWillDisappear(animated)
        refreshTimer?.invalidate()
        refreshTimer = nil
    }

    @objc private func refresh() {
        controller.fetchDiagnosticLog { [weak self] text in
            guard let self = self else {
                return
            }
            let appPart = "== app\n" + self.controller.eventLines.joined(separator: "\n")
            let extensionPart = text ?? self.controller.failureLog
                ?? NSLocalizedString("diagnostics.notRunning", comment: "")
            self.textView.text = appPart
                + "\n\n== extension (live)\n" + self.controller.diagnosticReceiver.text
                + "\n\n== extension (reply)\n" + extensionPart
        }
    }

    @objc private func copyTapped() {
        UIPasteboard.general.string = textView.text
    }

    @objc private func closeTapped() {
        dismiss(animated: true)
    }
}
