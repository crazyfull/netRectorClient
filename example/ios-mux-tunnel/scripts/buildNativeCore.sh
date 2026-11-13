#!/usr/bin/env bash
# Builds Native/MuxTunnelCore.xcframework: the mux-tunnelClient core (netRectorClient) and
# hev-socks5-tunnel merged into one static library, for iOS devices and the simulator.
# Run on macOS with Xcode, after `git submodule update --init --recursive`.
set -euo pipefail

# keep in sync with deploymentTarget in project.yml
readonly MIN_IOS_VERSION="${MIN_IOS_VERSION:-12.0}"
readonly PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly REPOSITORY_DIR="$(cd "$PROJECT_DIR/../.." && pwd)"
readonly HEV_DIR="$REPOSITORY_DIR/third_party/hev-socks5-tunnel"
readonly WORK_DIR="$PROJECT_DIR/build/native"
readonly OUTPUT="$PROJECT_DIR/Native/MuxTunnelCore.xcframework"

if [ ! -f "$HEV_DIR/Makefile" ]; then
    echo "hev-socks5-tunnel is missing: run 'git submodule update --init --recursive'" >&2
    exit 1
fi

# buildSlice <sdk> <arch>  ->  $WORK_DIR/slices/<sdk>-<arch>/libMuxTunnelCore.a
buildSlice() {
    local sdk="$1" arch="$2"
    local buildDir="$WORK_DIR/cmake-$sdk-$arch"
    local sliceDir="$WORK_DIR/slices/$sdk-$arch"
    local versionFlag="-m${sdk}-version-min=$MIN_IOS_VERSION"
    [ "$sdk" = "iphonesimulator" ] && versionFlag="-mios-simulator-version-min=$MIN_IOS_VERSION"

    echo "== core: $sdk $arch"
    cmake -S "$PROJECT_DIR/native" -B "$buildDir" \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_SYSROOT="$sdk" \
        -DCMAKE_OSX_ARCHITECTURES="$arch" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_IOS_VERSION" \
        -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$buildDir" --target muxTunnelCore -j "$(sysctl -n hw.ncpu)"

    echo "== hev-socks5-tunnel: $sdk $arch"
    make -C "$HEV_DIR" clean >/dev/null 2>&1 || true
    make -C "$HEV_DIR" \
        PP="xcrun --sdk $sdk --toolchain $sdk clang" \
        CC="xcrun --sdk $sdk --toolchain $sdk clang" \
        CFLAGS="-arch $arch $versionFlag" \
        LFLAGS="-arch $arch $versionFlag -Wl,-Bsymbolic-functions" \
        static

    mkdir -p "$sliceDir"
    libtool -static -o "$sliceDir/libMuxTunnelCore.a" \
        "$buildDir/libmuxTunnelCore.a" \
        "$(find "$buildDir" -name libnetRectorClient.a | head -n 1)" \
        "$HEV_DIR/bin/libhev-socks5-tunnel.a" \
        "$HEV_DIR/third-part/lwip/bin/liblwip.a" \
        "$HEV_DIR/third-part/yaml/bin/libyaml.a" \
        "$HEV_DIR/third-part/hev-task-system/bin/libhev-task-system.a"
    make -C "$HEV_DIR" clean >/dev/null 2>&1 || true
}

rm -rf "$WORK_DIR" "$OUTPUT"
mkdir -p "$WORK_DIR"

buildSlice iphoneos arm64
buildSlice iphonesimulator arm64
buildSlice iphonesimulator x86_64

# one library for both simulator architectures
mkdir -p "$WORK_DIR/slices/iphonesimulator-universal"
lipo -create \
    "$WORK_DIR/slices/iphonesimulator-arm64/libMuxTunnelCore.a" \
    "$WORK_DIR/slices/iphonesimulator-x86_64/libMuxTunnelCore.a" \
    -output "$WORK_DIR/slices/iphonesimulator-universal/libMuxTunnelCore.a"

# headers + the module map that defines the Swift modules MuxTunnelCore and HevSocks5Tunnel
readonly INCLUDE_DIR="$WORK_DIR/include"
mkdir -p "$INCLUDE_DIR"
cp "$PROJECT_DIR/native/MuxTunnelCore.h" "$PROJECT_DIR/native/module.modulemap" "$INCLUDE_DIR/"
cp "$HEV_DIR/src/hev-main.h" "$INCLUDE_DIR/"

mkdir -p "$PROJECT_DIR/Native"
xcodebuild -create-xcframework \
    -library "$WORK_DIR/slices/iphoneos-arm64/libMuxTunnelCore.a" -headers "$INCLUDE_DIR" \
    -library "$WORK_DIR/slices/iphonesimulator-universal/libMuxTunnelCore.a" -headers "$INCLUDE_DIR" \
    -output "$OUTPUT"

echo "Done: $OUTPUT"
