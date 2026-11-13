import SwiftUI

@main
struct MuxTunnelApp: App {
    @StateObject private var controller = TunnelController()

    var body: some Scene {
        WindowGroup {
            MainView()
                .environmentObject(controller)
        }
    }
}
