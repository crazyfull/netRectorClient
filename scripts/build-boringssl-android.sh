#!/usr/bin/env bash
# Builds BoringSSL (libssl.a, libcrypto.a) for every Android ABI the app ships, into
#   example/android-mux-tunnel/boringssl/{include, lib/<abi>/}
# which example/android-mux-tunnel/app/src/main/jni/Android.mk picks up automatically.
#   ANDROID_NDK_HOME  the NDK (the version in app/build.gradle)
#   BORINGSSL_SRC     BoringSSL source (default: fetched by scripts/fetch-boringssl.sh)
set -euo pipefail

readonly REPOSITORY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly OUT="$REPOSITORY_DIR/example/android-mux-tunnel/boringssl"
readonly SRC="${BORINGSSL_SRC:-$REPOSITORY_DIR/build/boringssl-src}"
readonly API_LEVEL="${ANDROID_API_LEVEL:-21}"
readonly NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"

if [ -z "$NDK" ] || [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
    echo "set ANDROID_NDK_HOME to the NDK directory" >&2
    exit 1
fi
"$REPOSITORY_DIR/scripts/fetch-boringssl.sh" "$SRC"

rm -rf "$OUT"
mkdir -p "$OUT/include"
cp -R "$SRC/include/openssl" "$OUT/include/"

for abi in armeabi-v7a arm64-v8a x86 x86_64; do
    echo "== BoringSSL: $abi"
    build="$REPOSITORY_DIR/build/boringssl-android-$abi"
    cmake -S "$SRC" -B "$build" \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$API_LEVEL" \
        -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF >/dev/null
    cmake --build "$build" --target ssl crypto --parallel 4
    mkdir -p "$OUT/lib/$abi"
    cp "$build/libssl.a" "$build/libcrypto.a" "$OUT/lib/$abi/"
done
echo "Done: $OUT"
