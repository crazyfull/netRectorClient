import SwiftUI

/// The only screen: status bar, settings and the Start / Stop button.
struct MainView: View {
    @EnvironmentObject private var controller: TunnelController

    var body: some View {
        NavigationView {
            Form {
                Section {
                    Button(action: controller.toggle) {
                        Text(LocalizedStringKey(controller.isActive ? "button.stop" : "button.start"))
                            .frame(maxWidth: .infinity)
                            .font(.headline)
                    }
                    .tint(controller.isActive ? .red : .accentColor)
                }

                if let message = controller.errorMessage {
                    Section {
                        Text(message)
                            .foregroundColor(.red)
                    }
                }

                ServerSettingsSection(configuration: $controller.configuration, isEditable: controller.isEditable)
                HandshakeSettingsSection(configuration: $controller.configuration, isEditable: controller.isEditable)

                Section(footer: Text("note.tcpOnly")) {
                    EmptyView()
                }
            }
            .navigationTitle("Mux Tunnel")
            .navigationBarTitleDisplayMode(.inline)
            .safeAreaInset(edge: .top, spacing: 0) {
                StatusBarView(status: controller.status)
            }
        }
        .navigationViewStyle(.stack)
    }
}
