# Mux Tunnel for iOS

The iOS version (**iOS 12.0 and later**, UIKit — SwiftUI would need iOS 13/14/15) of the [Android app](../android-mux-tunnel): the whole phone uses the
[mux-tunnelClient](../mux-tunnelClient) tunnel as a VPN. A status bar at the top of the screen shows
the state and **how many tunnels are really connected** (for example `Tunnels 2 / 3`).

```
apps -> utun -> hev-socks5-tunnel -> SOCKS5 127.0.0.1:<port> -> mux-tunnelClient core -> CDN -> mux-tunnelServer
          \_______________ Packet Tunnel extension (MuxTunnelExtension) ______________/
```

## Layout

| Folder | Content |
|---|---|
| `native/` | C interface of the core (`MuxTunnelCore.h`), CMake file; built with `netRectorClient` and the example sources |
| `Shared/` | types used by both targets: `TunnelConfiguration`, `TunnelStatus`, `TunnelConnectionState`, `TunnelProviderRequest`, … |
| `Extension/` | the Packet Tunnel extension: `PacketTunnelProvider`, `MuxCoreRunner`, `HevTunnelRunner`, `HevConfigurationBuilder`, … |
| `App/` | UIKit app: `MainViewController`, `StatusBarView`, `LabeledTextField`, `TunnelCountView`, `TunnelController`, … |
| `scripts/buildNativeCore.sh` | builds `Native/MuxTunnelCore.xcframework` (core + hev-socks5-tunnel, device and simulator) |
| `project.yml` | [XcodeGen](https://github.com/yonaskolb/XcodeGen) description of the Xcode project |

Each class / struct / enum is in its own file; Swift naming (PascalCase types, camelCase members).

## How the status bar gets the real tunnel count

The extension is a separate process. The app sends `TunnelProviderRequest.status` to it once a
second (`sendProviderMessage`); the extension answers with a `TunnelStatus` built from
`muxTunnelCoreConnectedTunnels()`, which counts the tunnels that finished the handshake right now.
Colors: green = all tunnels up, orange = starting or only some up, red = none up / failed, gray = stopped.

## Build

Needs macOS, Xcode 15 or 16, CMake and XcodeGen (`brew install xcodegen`):

```sh
git submodule update --init --recursive
cd example/ios-mux-tunnel
./scripts/buildNativeCore.sh      # -> Native/MuxTunnelCore.xcframework
xcodegen generate                 # -> MuxTunnel.xcodeproj
open MuxTunnel.xcodeproj
```

In Xcode set your **Team** for both targets. A VPN app needs the **Network Extensions** capability
(Packet Tunnel) — a paid Apple Developer account. If you change the bundle ids, change them in
`project.yml` and in `DefaultTunnelSettings.swift` (`appBundleIdentifier`, `extensionBundleIdentifier`).

### GitHub Actions

*Actions → iOS IPA (unsigned) → Run workflow* builds the native core and the app and uploads an
**unsigned** IPA (optionally as a release). Sign it with your own provisioning profiles
(both bundle ids, with the Network Extensions capability) using Xcode, fastlane or a sideloading tool.

## Notes

* Only TCP traffic and DNS go through the tunnel (mapped DNS of hev-socks5-tunnel); other UDP is dropped.
* A Network Extension has a small memory limit, so the engine runs with small buffers
  (`HevConfigurationBuilder`). Raise the number of tunnels carefully on old devices.
* The tun file descriptor is taken from `packetFlow` (`socket.fileDescriptor`), with a fallback scan
  for the utun control socket.
* Verification status: the native core and its C interface were built and tested on Linux, including
  the connected-tunnel counter. The Swift code, the Xcode project and the iOS build could **not** be
  compiled in the authoring environment (no macOS / Xcode): expect to fix small compile errors on the
  first run of the workflow and send the log.
