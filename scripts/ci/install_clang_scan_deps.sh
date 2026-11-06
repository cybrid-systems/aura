#!/bin/bash
# Install clang-scan-deps from the official LLVM 23.1.5 release.
# aura-ci:v0.1.0's clang++ is that release (commit 392dfff97d29) but the
# image does not ship the scanner. CMake's Clang module BMI build refuses
# to run without it. Do not substitute another Clang major.
set -euo pipefail

expected_ver=23.1.5
expected_commit=392dfff97d29b59d77e5cac6d4b6ef2585fe7117
compiler=${CXX:-clang++}

ver=$("$compiler" --version | head -1)
echo "compiler: $ver"
echo "$ver" | grep -F "$expected_ver" >/dev/null
echo "$ver" | grep -F "$expected_commit" >/dev/null

check_installed() {
    local out
    out=$(clang-scan-deps --version 2>&1 || true)
    echo "clang-scan-deps: $out"
    # The official 23.1.5 tarball prints "LLVM version 23.1.5" and does not
    # repeat the git commit. The download URL is llvmorg-23.1.5, which is
    # commit 392dfff97d29. Refuse any other version string.
    echo "$out" | grep -F "LLVM version $expected_ver" >/dev/null
}

cache=${SCAN_DEPS_CACHE:-}
if [[ -n "$cache" && -x "$cache/clang-scan-deps" ]]; then
    sudo install -m 755 "$cache/clang-scan-deps" /usr/local/bin/clang-scan-deps
    if check_installed; then
        exit 0
    fi
    echo "cached clang-scan-deps did not match $expected_ver $expected_commit" >&2
fi

case "$(uname -m)" in
    x86_64) asset=LLVM-23.1.5-Linux-X64.tar.xz ;;
    aarch64 | arm64) asset=LLVM-23.1.5-Linux-ARM64.tar.xz ;;
    *)
        echo "unsupported architecture $(uname -m)" >&2
        exit 1
        ;;
esac

if ! command -v curl >/dev/null || ! command -v xz >/dev/null; then
    sudo mkdir -p /var/lib/apt/lists/partial
    sudo DEBIAN_FRONTEND=noninteractive apt-get update -qq
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends curl xz-utils ca-certificates
fi

url="https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.5/${asset}"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
echo "download $url"
curl -fL --retry 3 --retry-delay 2 -o "$tmp/llvm.tar.xz" "$url"
tar -xJf "$tmp/llvm.tar.xz" -C "$tmp" --wildcards '*/bin/clang-scan-deps'
bin=$(find "$tmp" -type f -name clang-scan-deps -print -quit)
test -n "$bin"
if [[ -n "$cache" ]]; then
    mkdir -p "$cache"
    cp "$bin" "$cache/clang-scan-deps"
    chmod 755 "$cache/clang-scan-deps"
fi
sudo install -m 755 "$bin" /usr/local/bin/clang-scan-deps
check_installed
