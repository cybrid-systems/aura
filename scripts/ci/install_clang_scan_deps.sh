#!/bin/bash
# Put clang-scan-deps 23.1.5 next to this job's clang++.
#
# If a scanner is already installed and it prints LLVM 23.1.5, keep that
# binary. Do not replace it from the actions cache and do not download LLVM.
# A missing scanner, or one that is not 23.1.5, uses the official
# llvmorg-23.1.5 tarball (the same commit aura-ci's clang++ reports).
# Another Clang major is not a substitute.
set -euo pipefail

expected_ver=23.1.5
expected_commit=392dfff97d29b59d77e5cac6d4b6ef2585fe7117
compiler=${CXX:-clang++}
dest=${SCAN_DEPS_DEST:-/usr/local/bin/clang-scan-deps}

ver=$("$compiler" --version | head -1)
echo "compiler: $ver"
if ! echo "$ver" | grep -F "$expected_ver" >/dev/null || ! echo "$ver" | grep -F "$expected_commit" >/dev/null; then
    echo "clang++ must be $expected_ver ($expected_commit); got: $ver" >&2
    exit 1
fi

# Official 23.1.5 tarball prints "LLVM version 23.1.5" and does not repeat
# the git commit. The download URL is llvmorg-23.1.5, which is commit
# 392dfff97d29. Any other version string is refused.
scanner_matches() {
    local bin=$1
    local out
    [[ -n "$bin" && -x "$bin" ]] || return 1
    out=$("$bin" --version 2>&1 || true)
    echo "clang-scan-deps ($bin): $out"
    echo "$out" | grep -F "LLVM version $expected_ver" >/dev/null
}

install_bin() {
    local src=$1
    local dir
    dir=$(dirname "$dest")
    mkdir -p "$dir"
    if [[ -w "$dir" ]]; then
        install -m 755 "$src" "$dest"
    else
        sudo install -m 755 "$src" "$dest"
    fi
}

# Image (or an earlier step) already has a matching scanner. Leave it.
if scanner_matches "$dest"; then
    echo "using installed clang-scan-deps at $dest"
    exit 0
fi
if command -v clang-scan-deps >/dev/null 2>&1; then
    found=$(command -v clang-scan-deps)
    if [[ "$found" != "$dest" ]] && scanner_matches "$found"; then
        echo "using installed clang-scan-deps at $found"
        install_bin "$found"
        exit 0
    fi
    echo "installed clang-scan-deps does not match LLVM $expected_ver" >&2
fi

cache=${SCAN_DEPS_CACHE:-}
if [[ -n "$cache" && -x "$cache/clang-scan-deps" ]]; then
    install_bin "$cache/clang-scan-deps"
    if scanner_matches "$dest"; then
        echo "using cached clang-scan-deps"
        exit 0
    fi
    echo "cached clang-scan-deps did not match LLVM $expected_ver" >&2
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
install_bin "$bin"
scanner_matches "$dest"
