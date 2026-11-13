import SwiftUI

/// CDN / server address, port and number of tunnels.
struct ServerSettingsSection: View {
    @Binding var configuration: TunnelConfiguration
    let isEditable: Bool

    var body: some View {
        Section(header: Text("section.server")) {
            TextField("field.address", text: $configuration.cdnAddress)
                .keyboardType(.URL)
                .textInputAutocapitalization(.never)
                .disableAutocorrection(true)
                .disabled(!isEditable)

            TextField("field.port", value: $configuration.cdnPort, format: .number.grouping(.never))
                .keyboardType(.numberPad)
                .disabled(!isEditable)

            Stepper(value: $configuration.tunnelCount,
                    in: DefaultTunnelSettings.minTunnelCount...DefaultTunnelSettings.maxTunnelCount) {
                Text(String(format: NSLocalizedString("field.tunnels", comment: ""), configuration.tunnelCount))
            }
            .disabled(!isEditable)
        }
    }
}
