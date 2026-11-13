# Third-party licenses

netRector itself is licensed under LGPL-2.1-or-later (see `LICENCE`). It includes or uses the following third-party code.

| Component | Where | License |
|---|---|---|
| TLSF allocator (Matthew Conte) | `src/tlsf.c`, `src/tlsf.h` (vendored) | BSD 3-Clause. Full text in the header comment of `src/tlsf.h`; keep it when redistributing. |
| BoringSSL | not included; linked only when TLS is enabled (`libs/boringssl` or `BORINGSSL_DIR`) | Its own license terms; see the `LICENSE` file in the BoringSSL source tree. |
| hashicorp/yamux v0.1.2 | not included; downloaded only to build the test helper `tests/go_yamux/goyamux` | MPL-2.0 (see the module's own `LICENSE`). |

The Yamux wire protocol implemented in `src/clsMultiplexedTunnel.*` follows the public specification at https://github.com/hashicorp/yamux/blob/master/spec.md; the repository contains no hashicorp/yamux source files (only a comment and the spec link refer to it).

## Android and iOS examples (`example/android-mux-tunnel`, `example/ios-mux-tunnel`)

| Component | Where | License |
|---|---|---|
| hev-socks5-tunnel (heiher) | git submodule `third_party/hev-socks5-tunnel`, built into the apps (`libhev-socks5-tunnel.so` on Android, a static library on iOS) | MIT. Its own submodules `hev-task-system` and `yaml` are MIT too; `lwip` is BSD-3-Clause. License files are in the submodule trees. |
