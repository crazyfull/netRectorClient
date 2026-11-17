# iOS guide: a VPN on netRectorClient (Packet Tunnel + mux)

Everything learned while bringing `example/ios-mux-tunnel` from "it builds" to "traffic goes through the
tunnel" on a real iPhone. Read this **before** writing the real VPN app. It lists what is verified, what
failed and why, what to do, what not to do, and what is still unknown.

Verified on: iPhone 6s, iOS 15, installed with TrollStore (no paid Apple Developer account), LTE and Wi-Fi.
Last on-device check: build `m` (hev connects to the core over `::1`, real flows such as `bag.itunes.apple.com:443`
reach the core, 2 MB memory, 1 tunnel). Build `n` (VPN announced only after the mux tunnel is up) is pushed but not yet
checked on the device.
Not verified: any other device or iOS version, long running behavior, battery, throughput, network
changes (Wi-Fi <-> LTE), sleep/wake, memory under load. Treat those as open work.

---

## 1. Architecture

```
 apps ──► utun (system) ──► hev-socks5-tunnel (tun2socks, in the extension)
                                   │  SOCKS5 over ::1:<random port> (127.0.0.1 as fallback)
                                   ▼
                        netRectorClient core (mux-tunnelClient)  ──► CDN / server  (real network)
```

- **App** (`MuxTunnel`): UI only. It saves the VPN configuration (`NETunnelProviderManager`), starts and
  stops the tunnel, shows the status. It must not carry the traffic.
- **Packet Tunnel extension** (`MuxTunnelExtension`): a **separate process**. It runs hev-socks5-tunnel
  and the mux core (`example/mux-tunnelClient` through the C API in `native/MuxTunnelCore.h`).
  All VPN logic lives here. Hard memory limit (section 6).
- **hev-socks5-tunnel** reads IP packets from the utun file descriptor, terminates TCP in user space
  (lwIP), and opens one SOCKS5 connection per TCP flow to the local core. DNS is answered by hev itself
  (mapped DNS, `198.18.0.2`), so apps connect by domain through the tunnel.
- **Core**: SOCKS5 listener on **both loopbacks** (`LoopbackListener`: 127.0.0.1 and ::1, one port), N mux tunnels to the server (websocket-style handshake first), round-robin.
- TCP only. UDP is not carried by the mux (section 7).

Code map: `Shared/` (types used by both processes), `Extension/` (provider, engines, diagnostics),
`App/` (UI, controller, diagnostics screen), `native/` (C API + CMake), `scripts/buildNativeCore.sh`,
`patches/` (hev patch), `project.yml` (XcodeGen).

---

## 2. Distribution without a paid account (what actually works)

| Goal | Result |
|---|---|
| Sideloadly / free Apple ID | **Cannot install a VPN app.** A free account has no *Network Extension* entitlement; install fails with a permission error. |
| Unsigned IPA | Installs nowhere without signing. |
| **TrollStore** | **Works.** Entitlements are embedded with a fake signature (`ldid -S`), no profile needed. CI produces `MuxTunnel-trollstore.ipa`. |
| TestFlight / App Store | Needs a paid account and Apple's approval of the Network Extension capability for **both** bundle ids (`io.github.crazyfull.muxtunnel`, `...muxtunnel.tunnel`). |
| A friend with a Mac + Xcode + paid account | Clone, `./scripts/buildNativeCore.sh`, `xcodegen generate`, open, sign with their team, run on a device. |

Installing TrollStore itself on iOS 15 from Windows: TrollInstallerX (Sideloadly + full iTunes). Steps in the
chat history, not repeated here; they depend on the exact iOS version.

CI (`.github/workflows/ios-build.yml`, manual): `macos-latest` (macOS 14 images fail with "configuration not
supported"), XcodeGen, xcframework, `xcodebuild` unsigned, `ldid` for both binaries, **one non-zipped IPA**
(`upload-artifact@v7`, `archive: false`). Both entitlement files must contain
`com.apple.developer.networking.networkextension = [packet-tunnel-provider]`; the CI log prints them with `ldid -e`.

---

## 3. Problems met, in the order they appeared (symptom -> cause -> fix)

Each of these cost hours. Do them right the first time.

### 3.1 Status bar never showed a count, no error text (iOS < 16)
- `NEVPNConnection.fetchLastDisconnectError` exists only on **iOS 16+**. On iOS 15 and below a failing
  tunnel simply goes to "disconnected" with no reason.
- **Fix / rule:** never depend on the system for the reason. The extension must report its own state and
  reasons (section 5).

### 3.2 `sendProviderMessage` got no answer
- With the TrollStore install, `session.sendProviderMessage` returned `nil` while the tunnel was connected,
  every time. Cause not found (extension reachable, message channel not). Real signing may behave differently.
- **Fix:** a **loopback UDP channel** (`Shared/DiagnosticChannel`, `127.0.0.1:47321`): the extension sends log
  lines and a status datagram (`#status running 1 1`) every second; the app listens (`DiagnosticReceiver`).
  It works even if the extension dies right after. **Dev-only:** any local app can read it; gate it behind a debug
  flag or remove it for a production build, and keep `sendProviderMessage` as the production path.

### 3.3 The server's own connection looped into the tunnel (most important one)
- The tunnel installs a default route into the utun. The extension's connection to the CDN/server is **not
  automatically exempt**: it was captured by hev, sent to the local SOCKS5, rejected ("no tunnel available"),
  retried with backoff: `tunnel 0 lost` forever, +3 packets on the utun counter per attempt.
- **Fix:** exclude the server IP: `NEIPv4Settings.excludedRoutes = [NEIPv4Route(server, 255.255.255.255)]`
  (IPv6: prefix 128). Verify with a probe: the socket's **local address** must be the Wi-Fi/LTE address, not
  `198.18.0.1` (`ConnectivityProbe`, log lines `probe[...]`).
- **Resolve the server name before** `setTunnelNetworkSettings`. Afterwards the system resolver of the
  extension asks the tunnel's DNS, which answers with mapped addresses that lead back into the tunnel.
  Pass the resolved **IP** to the core for its whole life. Re-resolve on the next start.
- **Start the core after the settings are applied**, so its first connect already sees the final routes.
- **Do not announce the VPN before the mux tunnel is up** (3.8).
- If the physical network has **no route to the server**, the connect falls through to the default route
  (the tunnel) and you get the same endless loop, plus `ENETUNREACH` on a probe before the VPN. Seen with a
  CDN IP that was not reachable from that ISP/Wi-Fi. A reachability probe before bringing the VPN up should
  show a clear error ("server not reachable from this network") instead of a loop. (Not implemented yet.)

### 3.4 hev could not connect to the local SOCKS5 listener (EADDRNOTAVAIL)
- Symptom: tunnel `1/1`, utun packets flowing, hev log full of `socks5 client connect` (failure, no
  `server fd`), the core never sees a stream. A plain `AF_INET` connect from the same process worked in 0 ms.
- Cause: hev creates an **`AF_INET6` socket with `IPV6_V6ONLY=0` and connects to the IPv4-mapped address**
  `::ffff:127.0.0.1`. Inside the iOS Network Extension this fails with errno 49 (`EADDRNOTAVAIL`).
  The same code works on Linux/Android **and on a macOS CI runner** (so a macOS test does not prove iOS).
- **Fix:** `patches/hev-core-ios-ipv4-socks.patch` (applied by `scripts/buildNativeCore.sh` to the nested
  `third_party/hev-socks5-tunnel/src/core`, idempotent): on Apple platforms an IPv4 SOCKS server gets an `AF_INET`
  socket. Re-check the patch whenever the hev submodule is updated.
- **Preferred way (build `m`+):** netRectorClient's `LoopbackListener` listens on `127.0.0.1` **and** `::1` on one port,
  and the extension points hev at `::1` (`LoopbackPortFinder.findEndpoint()`). A plain `AF_INET6` connect to `::1` is
  verified to work in the extension (probe: connected in 0 ms), so hev needs no patch for it. The patch stays as the
  fallback: if `::1` cannot be bound, the engine gets `127.0.0.1` and the patched `AF_INET` path.

- Verified in the build `m` log: `the engine connects to ::1`, hev `socks5 client connect [::1]:<port>`, flows reach the
  core; the probe `::ffff:127.0.0.1` still fails with error 49 (so the IPv4-mapped form really is the iOS problem).
  The patch code path is not used when the address is `::1`; it is only the fallback.
- `socks5 client res.rep 7` in the hev log is expected: hev asks for UDP ASSOCIATE and the core refuses it (section 7).

### 3.5 Quick start/stop shows a false error
- `saveToPreferences` makes the system post `.disconnected` **before** `.connecting`. Treating "disconnected while
  starting" as a failure produced a red "stopped right after starting" on every successful start.
- **Rule:** only call it a failure after the system reported connecting/connected (`hasSeenConnection`) and the
  user did not press Stop.

### 3.6 Extension dies and takes its log with it
- When an engine exits, `fail()` keeps the process alive for 60 s so the app can read the log, then
  `cancelTunnelWithError`. Without this a crash/exit leaves nothing to look at on iOS < 16.

### 3.7 hev log file grows across runs
- hev appends. `HevConfigurationBuilder.writeFile` deletes the file on every start; `HevLogReader` forwards new lines
  to the diagnostic log as `hev: ...`. `log-level: debug` is for diagnostic builds only (very chatty).

### 3.8 First connections rejected right after start ("no tunnel available")
- The VPN used to be announced as soon as the settings were applied. Apps and hev opened connections within 20 ms,
  before the mux tunnel was established (~0.3 s); the core answered `no tunnel available, rejecting connection`
  (hev: `res.rep 1`). Harmless (apps retry) but ugly, and the status said "connected" too early.
- **Fix (build `n`):** after the settings and the core start, `waitForMuxTunnel` polls `connectedTunnels` (50 ms, 15 s
  limit); only then hev starts and `completionHandler(nil)` is called. On timeout the start fails with
  `The server did not answer in time`, if the core stops meanwhile with `The mux client stopped`.

### 3.9 Smaller ones
- Swift: `strdup` in a `.map` needs an explicit closure type (`(String) -> UnsafeMutablePointer<CChar>`).
- hev's nested submodule: `git submodule update --init --recursive` (`src/core`, `third-part/*`). The CI checkout uses `submodules: recursive`.
- Android parity: Android excludes the app itself (`addDisallowedApplication`); **iOS cannot** (per-app VPN needs MDM).
  iOS needs the route exclusions above.

---

## 4. Do / Don't

**Do**
- Resolve the server to an IP before the tunnel settings, exclude that IP, start the core after the settings.
- Probe from inside the extension (connect, print errno and the socket's local address) before theorizing.
- Report state and reasons from the extension yourself; assume no system error text below iOS 16.
- Keep the extension tiny: no UI frameworks, no big allocations, no logging flood in release.
- Test on a real device; the simulator cannot run Network Extensions.
- Keep both processes' entitlements identical in the files and in the signed binaries (`ldid -e`).
- Pin hev; keep the patch next to it and re-apply on update.
- Start order (build `n`+): resolve the server, apply the network settings (server excluded), start the mux core, **wait until a mux tunnel is connected**
  (15 s limit, then an error), and only then start hev and call `completionHandler(nil)`. The VPN indicator therefore
  appears when the tunnel really works, and no early connection hits "no tunnel available".

**Don't**
- Don't assume the extension's own sockets bypass the tunnel. They do not.
- Don't let apps resolve the server name after the VPN is up.
- Don't trust "works on macOS" for socket behavior inside a Network Extension.
- Don't put real server addresses in public docs/README (use `irc.cdn.ir`, `sv1.server.ir`).
- Don't ship the loopback UDP log channel or `log-level: debug` in production.
- Don't iterate by sending the user new builds without a probe that answers the open question.

---

## 5. Diagnostics toolkit (already in the project)

Diagnostics screen (button at the bottom of the main screen) shows: `== app` events (system status changes),
`== extension (live)` (lines over the UDP channel), `== extension (reply)` (only if provider messages work).

Healthy start looks like this (build stamp first, then in this order):

```
startTunnel, build <stamp>
server <host> -> <ip>:<port>, tunnels N
socks5 listener port <port>, the engine connects to ::1
probe[before vpn] ... connected in ~100 ms, local address <wifi/lte address>   <- NOT 198.18.0.1
network settings applied, server <ip> excluded from the routes
core: connecting tunnel 0 to <ip>:<port>
core: SOCKS5 on port <port> (IPv4 loopback: yes, IPv6 loopback: yes), ...
probe[after vpn]  ... connected, local address <wifi/lte address>
core: connected to <ip>:<port>[, TLS handshake done: TLSv1.3], sending the handshake request
core: tunnel established with <ip>:<port>
mux tunnel is up, starting the engine
utun descriptor 5
tunnel running
tick tunnels 1/1 utun->engine ..p engine->utun ..p mem ..MB                    <- every 3 s
hev: ... socks5 client tcp -> [host]:443 ... socks5 client connect server fd N   <- real flows
```

How to read it:

| You see | Meaning |
|---|---|
| `Network is unreachable` in `probe[before vpn]` | the phone has no route to that IP on this network |
| probe local address `198.18.0.1` | the server connection loops into the tunnel (exclusion missing/ineffective) |
| `tunnel N lost` repeating, `connected to` within 1-2 ms | loop into hev (real TCP to a remote takes >= tens of ms) |
| `closed while waiting for the handshake response (error 54)` | server/CDN closes during the websocket-style handshake: check Host/path/key/accept |
| `utun->engine` grows, no `stream opened` in core, hev `socks5 client connect` without `server fd` | hev cannot reach the SOCKS5 listener (section 3.4) |
| `utun->engine` does not grow while using apps | traffic does not enter the tunnel: routes/DNS |
| `start failed: The server did not answer in time` | no mux tunnel within 15 s: server/CDN unreachable or handshake rejected |
| TLS on: `TLS handshake done`, then `waiting for the handshake response (error 60)` | TLS is fine; the server/CDN does not answer the websocket-style request: check Host (it is also the SNI), path, key, port, or a middlebox that drops data after the handshake |
| `connect error 49` in a `probe[engine style ...]` line | expected: it only reproduces the old IPv4-mapped problem; not a failure |
| `res.rep 7` in the hev log | normal: the core refuses UDP ASSOCIATE |
| `extension does not answer status requests` | provider messages do not work on this install; the UDP channel still does |

---

## 6. Limits, numbers, settings

- Extension memory (jetsam): about **50 MB on iOS 15+**, about 15 MB on iOS 12-14 (from Apple behavior; the limit
  is process memory, **not** the size of linked libraries). Measured here: **2 MB** `phys_footprint` idle with
  1 tunnel. Under load: **not measured**. Log it (`tick ... mem`) when testing real traffic.
- hev (`HevConfigurationBuilder`): `task-stack-size: 86016` (hev default; 36864 was tried "to save memory" and
  never tested, do not go lower without testing), `tcp-buffer-size: 16384`, `max-session-count: 256`
  (about 60-70 KB per session is an estimate: raise only after measuring), `connect-timeout: 10000`,
  `tcp-read-write-timeout: 300000`.
- Tunnel interface: IPv4 `198.18.0.1/32`, IPv6 `fc00::1/128`, DNS `198.18.0.2` (answered by hev), mapped network
  `100.64.0.0/10`, MTU 1500.
- Tunnel count 1-16 (default 1), round-robin per SOCKS5 stream. Mux: 32 KB frames, 256 KB window.
- **Battery (not measured, reasoning only).** Idle cost comes from waking the CPU and the radio, not from the number
  of open sockets. Each tunnel sends a mux ping every `keepAliveSeconds` (30 s); with N tunnels that is up to N radio
  wake-ups per 30 s instead of 1, so 8 permanent tunnels cost clearly more than 1. Other idle wake-ups: the core loop
  runs in slices of at most `MaxLoopSliceMs` = 1000 ms (a wake-up per second even with no traffic), hev has its own
  timer task, and the diagnostic stats timer ticks every second (dev only). Under load the tunnel count barely
  matters: the same bytes use the radio either way. Ideas: keep 1-2 tunnels and open more only when streams pile up,
  line the pings of all tunnels up (or ping only one), raise the loop slice and keep-alive when the screen is off,
  remove the 1 s stats timer in release. Measure with Xcode Energy / Instruments before deciding.

---

## 7. Known gaps

- **UDP is not carried.** hev is configured with `udp: 'udp'`, so UDP flows ask the core for SOCKS5 UDP ASSOCIATE,
  which it refuses. DNS is unaffected (hev answers it). QUIC/HTTP3 falls back to TCP in Safari. For real UDP:
  implement UDP in the mux protocol or use hev's UDP-over-TCP mode on both ends.
- **TLS** (build `2026-11-17 a`+): the *Enable TLS* switch wraps the tunnels in TLS, SNI = the *Host* field (formerly
  "Handshake Host header"), certificate not verified. BoringSSL is **stock** and built per slice by
  `scripts/buildBoringSSL.sh`; the record-version customization (`TLS_CL_VERSION`) is skipped until the customized
  BoringSSL is used. **Not yet verified on a device or on a macOS runner**: check that the log shows
  `TLS enabled, server certificate is NOT verified` and that tunnels connect. An HTTP/TLS client for the real login
  can reuse the same BoringSSL.
- No reconnect logic on network change (Wi-Fi <-> LTE): the server exclusion route and the physical interface
  change; use `NWPathMonitor` in the extension and re-apply settings.
- No on-demand rules, no kill switch decision (`includeAllNetworks`), no IPv6 leak review.
- Reachability error message (3.3) and the split uplink/downlink tunnels are not implemented.

---

## 8. Building the real VPN app on top of this

1. **Account and signing first.** A paid account with the Network Extension capability (both targets), then keep
   TrollStore only as a test channel.
2. **Login and API calls from the app.** The app's own traffic **goes through the VPN** when it is on (no app
   exemption on iOS). Options: log in before connecting; or resolve the API host and add it to `excludedRoutes`
   like the server; or send the call through the tunnel (needs a working tunnel first).
3. **Where the user config lives** (about 1 KB JSON): `providerConfiguration` is enough for small data. A Keychain
   access group or an App Group is the usual way to share larger or secret data between app and extension;
   **untested under TrollStore**, test before designing around it.
4. **C++ side:** extend `MuxTunnelCore.h` (versioned C ABI): config in, status/error codes out, no C++ types across
   Swift. HTTP/TLS client in C++ needs BoringSSL for iOS. Keep the extension free of anything it does not need.
5. **Status and errors:** keep the extension-side reporting from this project (state, reasons, tunnel count). Replace
   the loopback UDP channel by `sendProviderMessage` plus Darwin notifications or the shared container once signing
   is real, and re-test.
6. **Test list before calling it done:** cold start, Stop/Start 20 times, airplane mode, Wi-Fi <-> LTE, lock screen
   for 30 min, low-memory warning, server unreachable (must show a clear error), server closes the handshake,
   reboot with the VPN profile installed, 16 tunnels, a large download, memory over time.

---

## 9. Quick reference

```sh
git submodule update --init --recursive
cd example/ios-mux-tunnel
./scripts/buildNativeCore.sh        # xcframework (applies patches/hev-core-ios-ipv4-socks.patch)
xcodegen generate
```

CI: Actions -> "iOS IPA (TrollStore)" -> Run workflow -> download `MuxTunnel-trollstore.ipa` (not zipped) ->
share to TrollStore -> Install. After installing, the build stamp is the first line of the diagnostic log
(`DefaultTunnelSettings.buildStamp`): bump it on every diagnostic build so you know which build runs.
