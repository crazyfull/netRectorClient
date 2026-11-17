#!/usr/bin/env bash
# Builds BoringSSL (libssl.a, libcrypto.a) for the iOS slices into build/native/boringssl/<sdk>-<arch>/
# (include/ and lib/ in each). Called by buildNativeCore.sh; run on macOS with Xcode.
#   BORINGSSL_SRC  BoringSSL source (default: fetched by ../../scripts/fetch-boringssl.sh)
set -euo pipefail

readonly MIN_IOS_VERSION="${MIN_IOS_VERSION:-12.0}"
readonly PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly REPOSITORY_DIR="$(cd "$PROJECT_DIR/../.." && pwd)"
readonly SRC="${BORINGSSL_SRC:-$REPOSITORY_DIR/build/boringssl-src}"
readonly OUT="${1:-$PROJECT_DIR/build/native/boringssl}"

"$REPOSITORY_DIR/scripts/fetch-boringssl.sh" "$SRC"

# buildSlice <sdk> <arch>
buildSlice() {
    local sdk="$1" arch="$2"
    local build="$OUT/cmake-$sdk-$arch"
    local dest="$OUT/$sdk-$arch"
    echo "== BoringSSL: $sdk $arch"
    cmake -S "$SRC" -B "$build" \
        -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$sdk" -DCMAKE_OSX_ARCHITECTURES="$arch" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_IOS_VERSION" \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF >/dev/null
    cmake --build "$build" --target ssl crypto --parallel 4
    mkdir -p "$dest/lib" "$dest/include"
    cp "$build/libssl.a" "$build/libcrypto.a" "$dest/lib/"
    cp -R "$SRC/include/openssl" "$dest/include/"
}

mkdir -p "$OUT"
buildSlice iphoneos arm64
buildSlice iphonesimulator arm64
buildSlice iphonesimulator x86_64
echo "Done: $OUT"
