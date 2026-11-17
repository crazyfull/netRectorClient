#!/usr/bin/env bash
# Downloads the pinned BoringSSL source into <dir> (default: build/boringssl-src).
#   scripts/fetch-boringssl.sh [dir]
# The mobile apps use a STOCK BoringSSL: the customized one (SSL_set_record_protocol_version, TLS_CL_VERSION)
# is not available to them yet. netRectorClient detects that and skips the customization (see CMakeLists.txt).
# When the customized BoringSSL can be used, point BORINGSSL_SRC at it and build with BORINGSSL_CUSTOM=1.
set -euo pipefail

# a commit that is known to build for Linux, Android and iOS; update on purpose, not by accident
readonly BORINGSSL_COMMIT="583980e24fddd61ebf6c82a7ce33caceb4e353ea"
readonly BORINGSSL_URL="${BORINGSSL_URL:-https://github.com/google/boringssl.git}"
readonly DIR="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build/boringssl-src}"

if [ -f "$DIR/include/openssl/ssl.h" ]; then
    echo "== BoringSSL source: $DIR (already there)"
    exit 0
fi

mkdir -p "$DIR"
git -C "$DIR" init -q
git -C "$DIR" remote add origin "$BORINGSSL_URL" 2>/dev/null || git -C "$DIR" remote set-url origin "$BORINGSSL_URL"
git -C "$DIR" fetch -q --depth 1 origin "$BORINGSSL_COMMIT"
git -C "$DIR" checkout -q FETCH_HEAD
echo "== BoringSSL source: $DIR at $BORINGSSL_COMMIT"
