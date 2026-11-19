# Mux Tunnel for Android

A small Android app (Android 5.0+ / API 21+) that makes the whole phone use the
[mux-tunnelClient](../mux-tunnelClient) tunnel as a VPN.

```
apps -> TUN -> hev-socks5-tunnel -> SOCKS5 127.0.0.1:<port> -> mux-tunnelClient core -> CDN -> mux-tunnelServer
```

* **mux-tunnelClient core** — the same C++ code as the desktop example (netRectorClient), built as
  `libmuxtunnel.so` with a small JNI bridge (`app/src/main/jni/muxtunnel_jni.cpp`).
* **[hev-socks5-tunnel](https://github.com/heiher/hev-socks5-tunnel)** — the tun2socks engine
  (git submodule). DNS queries are answered by its *mapped DNS*, so apps connect by domain name
  through the tunnel and no DNS leaks.
* **`TunnelVpnService`** — foreground `VpnService`. It holds a partial wake lock and a Wi-Fi lock,
  so the tunnel keeps working with the screen off, and restarts itself if Android kills it.
  The app itself is excluded from the VPN, so the CDN connections do not loop back.

## Using the app

1. Enter the CDN/server **address (IP or domain)**, **port** and the **number of tunnels** (1-16).
   The handshake fields (Host, path, key, accept) are under *Handshake (advanced)*.
   Tick **Enable TLS** to wrap the tunnel connections in TLS: the **Host** field is also the TLS server name
   (SNI; not sent for an IP address) and the server certificate is not verified. The status line shows how
   many tunnels are connected (`Tunnels 1 / 1`).
2. Press **Start**. Android asks once for VPN permission.
3. Press **Allow running in background** (shown while battery optimization is on) so the system
   does not stop the service when the screen is off. Some vendors (Xiaomi, Huawei, Samsung…) have
   an extra "auto-start / background" switch in their own settings.

TCP and DNS always go through the tunnel. Tick **Enable UDP** (off by default) to carry other UDP too
(games, calls): it is UDP-over-TCP on a **separate tunnel** that connects on the first UDP flow and closes after
60 s without datagrams (15 s while the screen is off; a running call keeps it open). The server must be a
mux-tunnelServer with UDP support. QUIC (HTTP/3) is not carried: the browser falls back to TCP. ICMP is not supported.
The Diagnostics tick line shows `udp tunnel up/down`.

Defaults live in `DefaultSettings.java` (mirrors `DefaultClientConfig.h`).

## Build

Needs JDK 17, the Android SDK and NDK r27 (`27.2.12479018`), and the submodules:

```sh
git submodule update --init --recursive
cd example/android-mux-tunnel
./gradlew assembleRelease          # or assembleDebug
```

APKs are written to `app/build/outputs/apk/<type>/`: one per ABI (`armeabi-v7a`, `arm64-v8a`,
`x86`, `x86_64`) and a universal APK.

### GitHub Actions

*Actions → Android APK → Run workflow* builds all ABIs and uploads the APKs as an artifact
(optionally also as a GitHub release). Without signing secrets the release APK is signed with the
debug key. To sign with your own key add the repository secrets `ANDROID_KEYSTORE_BASE64`,
`ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS` and `ANDROID_KEY_PASSWORD`.

## Notes

* The code uses only framework APIs (no AndroidX), `targetSdk 33`.
* **Diagnostics** (button on the main screen): the complete log without adb. Three parts: the app (service events,
  start settings, a direct probe of the server, network changes with Wi-Fi/mobile data and validation, a line every
  3 s with the connected tunnels, the engine's traffic counters and memory), the mux core (every connect, handshake,
  tunnel lost/reconnect) and the engine's own log file. `WARNING: data goes out but nothing comes back` means the
  tunnel looks up but carries no data. *Copy* / *Share* send the whole text. The log is kept while the app process
  lives (a stop/start keeps it); *Clear* wipes it.
* TLS is optional (*Enable TLS*, off by default). It needs BoringSSL built for the four ABIs:
  `ANDROID_NDK_HOME=... scripts/build-boringssl-android.sh` (the workflow does it) writes
  `example/android-mux-tunnel/boringssl/`, which `Android.mk` picks up. Without that directory the app builds
  without TLS and *Enable TLS* stops the tunnel with "TLS is not available in this build". The BoringSSL is a stock
  one: its record-version customization is skipped (see the main README).
* Off, the handshake and mux run over plain TCP, which is what a CDN in front of the server expects.
* Status of verification: the native core and the JNI bridge were tested on Linux against
  mux-tunnelServer; the APK itself is built by the workflow (it could not be built or run in the
  authoring environment, so report any Gradle/NDK error from the first run).
