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
2. Press **Start**. Android asks once for VPN permission.
3. Press **Allow running in background** (shown while battery optimization is on) so the system
   does not stop the service when the screen is off. Some vendors (Xiaomi, Huawei, Samsung…) have
   an extra "auto-start / background" switch in their own settings.

Only TCP traffic and DNS go through the tunnel (the mux server carries SOCKS5 CONNECT). Other UDP
traffic is not supported.

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
* TLS is not used between the app and the CDN (same as the example's default); the handshake and
  mux run over plain TCP, which is what a CDN in front of the server expects.
* Status of verification: the native core and the JNI bridge were tested on Linux against
  mux-tunnelServer; the APK itself is built by the workflow (it could not be built or run in the
  authoring environment, so report any Gradle/NDK error from the first run).
