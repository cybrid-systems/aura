#!/bin/bash
# Install clang-scan-deps for the clang++ this job actually runs.
#
# The version comes from `clang++ --version`. A scanner that already
# reports that same version is kept: nothing is downloaded and that
# file is not replaced. The actions cache is stored per version and
# architecture. The only fallback packages are the official assets
# LLVM-<version>-Linux-X64.tar.xz and LLVM-<version>-Linux-ARM64.tar.xz
# on the llvmorg-<version> release. Those names were checked on the
# llvmorg-20.1.8, 21.1.8, and 22.1.2 releases and on the later release
# that uses the same two filenames. Other archive names are not guessed. A compiler that is not a plain X.Y.Z release (a git
# snapshot or a release candidate) fails here instead of installing a
# different scanner.
set -euo pipefail

compiler=${CXX:-clang++}
dest=${SCAN_DEPS_DEST:-/usr/local/bin/clang-scan-deps}

ver_line=$("$compiler" --version | head -1)
echo "compiler: $ver_line" >&2

# "clang version X.Y.Z" and then end, space, or "(". A git snapshot
# (X.Y.Zgit) or a candidate (X.Y.Z-rc1) does not match.
if [[ "$ver_line" =~ clang\ version\ ([0-9]+\.[0-9]+\.[0-9]+)([^0-9]|$) ]]; then
    ver=${BASH_REMATCH[1]}
    rest=${BASH_REMATCH[2]}
    if [[ -n "$rest" && "$rest" != " " && "$rest" != "(" ]]; then
        ver=
    fi
else
    ver=
fi
if [[ -z "${ver:-}" ]]; then
    echo "cannot resolve an LLVM release from: $ver_line" >&2
    echo "refusing to install a scanner that may not match this clang++" >&2
    exit 1
fi
echo "matched clang++ LLVM version $ver" >&2
if [[ "${1:-}" == "--print-version" ]]; then
    echo "$ver"
    exit 0
fi

ver_re=${ver//./\\.}
scanner_matches() {
    local bin=$1
    local out
    [[ -n "$bin" && -x "$bin" ]] || return 1
    out=$("$bin" --version 2>&1 || true)
    echo "clang-scan-deps ($bin): $out"
    echo "$out" | grep -Eq "(LLVM|clang) version ${ver_re}([^0-9]|$)"
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

if scanner_matches "$dest"; then
    echo "using installed clang-scan-deps $ver at $dest"
    exit 0
fi
if command -v clang-scan-deps >/dev/null 2>&1; then
    found=$(command -v clang-scan-deps)
    if [[ "$found" != "$dest" ]] && scanner_matches "$found"; then
        echo "using installed clang-scan-deps $ver at $found"
        install_bin "$found"
        exit 0
    fi
    echo "installed clang-scan-deps does not match LLVM $ver" >&2
fi

arch=$(uname -m)
cache=${SCAN_DEPS_CACHE:-}
if [[ -n "$cache" && -x "$cache/$ver/$arch/clang-scan-deps" ]]; then
    install_bin "$cache/$ver/$arch/clang-scan-deps"
    if scanner_matches "$dest"; then
        echo "using cached clang-scan-deps $ver $arch"
        exit 0
    fi
    echo "cached clang-scan-deps for $ver $arch did not match" >&2
fi

case "$arch" in
    x86_64) asset="LLVM-${ver}-Linux-X64.tar.xz" ;;
    aarch64 | arm64) asset="LLVM-${ver}-Linux-ARM64.tar.xz" ;;
    *)
        echo "unsupported architecture $arch" >&2
        exit 1
        ;;
esac

if ! command -v curl >/dev/null || ! command -v xz >/dev/null; then
    sudo mkdir -p /var/lib/apt/lists/partial
    sudo DEBIAN_FRONTEND=noninteractive apt-get update -qq
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends curl xz-utils ca-certificates
fi

url="https://github.com/llvm/llvm-project/releases/download/llvmorg-${ver}/${asset}"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
echo "download $url"
curl -fL --retry 3 --retry-delay 2 -o "$tmp/llvm.tar.xz" "$url"
tar -xJf "$tmp/llvm.tar.xz" -C "$tmp" --wildcards '*/bin/clang-scan-deps'
bin=$(find "$tmp" -type f -name clang-scan-deps -print -quit)
test -n "$bin"
if [[ -n "$cache" ]]; then
    mkdir -p "$cache/$ver/$arch"
    cp "$bin" "$cache/$ver/$arch/clang-scan-deps"
    chmod 755 "$cache/$ver/$arch/clang-scan-deps"
fi
install_bin "$bin"
if ! scanner_matches "$dest"; then
    echo "downloaded clang-scan-deps does not match LLVM $ver" >&2
    exit 1
fi
echo "installed clang-scan-deps $ver from $url"
