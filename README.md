# netRectorClient

Static, minimal, cross-platform variant of [netRector](https://github.com/crazyfull/netRector) for
client applications: **Linux, Android, iOS/macOS, Windows**.

The public classes keep the names and the API of netRector (`TCPSocket`, `UDPSocket`, `Listener`,
`MultiplexedTunnel`, `TLSContext`, `SendQueue`, and `EpollReactor` as an alias of `EventLoop`), so an
application can switch between the two libraries by changing the include path and link target.

## What is different

| | netRector | netRectorClient |
|---|---|---|
| I/O | epoll, reactor shards, `Server` | one portable `EventLoop` (`poll()` on POSIX, `select()` on Windows), one loop = one thread |
| Timers | `addTimer` | none; the loop runs internal timeouts about once per second; apps that need a clock drive it with `runOnce(ms)` and `getCachedNowMs()` |
| DNS | c-ares / custom | `getaddrinfo` on worker threads |
| TCP, UDP, TLS (BoringSSL), mux | yes | yes |
| Linux-only extras (`recvmmsg`, buffer pool, async key worker) | yes | removed |

`EventLoop::post()`, `wake()` and `stop()` are thread-safe; everything else belongs to the loop thread.

## Build

```sh
cmake -S . -B build -DBORINGSSL_ROOT=/path/to/boringssl     # TLS is on when BoringSSL is found
cmake --build build -j
ctest --test-dir build                                       # needs the certs dir, see tests/
```

Options: `NETRECTOR_ENABLE_TLS`, `NETRECTOR_BUILD_EXAMPLES`, `NETRECTOR_BUILD_TESTS`.
The result is a static library `libnetRectorClient.a` (`netRectorClient.lib` on Windows).

* **Android**: NDK toolchain file, `-DANDROID_ABI=...`; links `log`. TLS trust store: `/system/etc/security/cacerts`.
* **iOS / macOS**: `-DCMAKE_SYSTEM_NAME=iOS` (or Apple toolchain). No system CA path: give a CA bundle to `TLSContext`.
* **Windows**: MSVC or MinGW; links `ws2_32`/`mswsock`; give a CA bundle to `TLSContext`.

## Usage

```cpp
#include "netRector.h"
using namespace netrector;

EventLoop loop;
MySocket* s = new MySocket;          // derives from TCPSocket
s->setReactor(&loop);
s->connectTo("example.com", 80);
std::atomic<bool> stop{false};
loop.run(stop);
```

## Examples

[`example/mux-tunnelClient`](example/mux-tunnelClient) — the netRector mux-tunnelClient (SOCKS5 over
mux tunnels) ported to this library.

[`example/android-mux-tunnel`](example/android-mux-tunnel) — Android app (5.0+) that runs the tunnel as
a system VPN (foreground service, works with the screen off), with a manual GitHub Actions build
for all ABIs. Clone with `--recurse-submodules` (it uses hev-socks5-tunnel).

[`example/ios-mux-tunnel`](example/ios-mux-tunnel) — the same for iOS (SwiftUI + Packet Tunnel extension),
with a status bar that shows how many tunnels are connected; manual GitHub Actions build (unsigned IPA).

## Status

Validated on Linux (functional tests, ASan/UBSan, interop with netRector's mux-tunnelServer).
Cross-compile checked for Windows (MinGW) and macOS (arm64). Android/iOS builds and Windows runtime
are not yet tested on real targets.
