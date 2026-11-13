import SwiftUI

/// Websocket-style handshake sent before the mux starts (advanced).
struct HandshakeSettingsSection: View {
    @Binding var configuration: TunnelConfiguration
    let isEditable: Bool

    var body: some View {
        Section(header: Text("section.handshake")) {
            field("field.handshakeHost", text: $configuration.handshakeHost)
            field("field.handshakePath", text: $configuration.handshakePath)
            field("field.webSocketKey", text: $configuration.webSocketKey)
            field("field.expectedAccept", text: $configuration.expectedAccept)
        }
    }

    private func field(_ title: LocalizedStringKey, text: Binding<String>) -> some View {
        TextField(title, text: text)
            .textInputAutocapitalization(.never)
            .disableAutocorrection(true)
            .disabled(!isEditable)
    }
}
