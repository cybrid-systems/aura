#!/bin/sh
# Issue #4907: install a prebuilt aura.
#   curl -fsSL https://raw.githubusercontent.com/cybrid-systems/aura/main/install.sh | sh
# Env: AURA_VERSION (release tag, default nightly), AURA_PREFIX (default
# ~/.aura), AURA_BIN_DIR (symlink dir, default ~/.local/bin),
# AURA_DOWNLOAD_BASE (default the GitHub release of AURA_VERSION).
set -eu
case "$(uname -s)" in Linux) ;; *) echo "aura install: only Linux is supported (got $(uname -s))" >&2; exit 1 ;; esac
case "$(uname -m)" in
  x86_64 | amd64) arch=x86_64 ;;
  aarch64 | arm64) arch=arm64 ;;
  *) echo "aura install: unsupported architecture $(uname -m) (x86_64 and arm64 only)" >&2; exit 1 ;;
esac
version=${AURA_VERSION:-nightly}
prefix=${AURA_PREFIX:-$HOME/.aura}
bindir=${AURA_BIN_DIR:-$HOME/.local/bin}
base=${AURA_DOWNLOAD_BASE:-https://github.com/cybrid-systems/aura/releases/download/$version}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fetch() {
  case "$1" in
    /* | file://*) cp "${1#file://}" "$2" ;;
    *) if command -v curl >/dev/null 2>&1; then curl -fsSL "$1" -o "$2"; else wget -qO "$2" "$1"; fi ;;
  esac
}
# Tag v1.2.3 ships aura-1.2.3-linux-<arch>.tar.gz (scripts/tools/release.sh).
name="aura-${version#v}-linux-$arch.tar.gz"
echo "aura install: $name ($arch) -> $prefix"
fetch "$base/$name" "$tmp/$name"
fetch "$base/$name.sha256" "$tmp/$name.sha256"
want=$(cut -d' ' -f1 "$tmp/$name.sha256")
if command -v sha256sum >/dev/null 2>&1; then got=$(sha256sum "$tmp/$name" | cut -d' ' -f1)
else got=$(shasum -a 256 "$tmp/$name" | cut -d' ' -f1); fi
[ "$want" = "$got" ] || { echo "aura install: sha256 mismatch for $name (want $want, got $got)" >&2; exit 1; }
mkdir -p "$prefix" "$bindir"
tar -xzf "$tmp/$name" -C "$tmp"
# the tarball's top directory is its own name (bundle.sh); no find(1) — minimal
# images such as openEuler do not ship findutils (#4934)
dir=$tmp/${name%.tar.gz}
[ -d "$dir" ] || { echo "aura install: $name has no top directory ${name%.tar.gz}/" >&2; exit 1; }
rm -rf "$prefix/current" && mv "$dir" "$prefix/current"
ln -sf "$prefix/current/bin/aura" "$bindir/aura"
echo "aura install: ok — $("$bindir/aura" -e '(display "aura runs")' 2>&1 | tail -1)"
case ":$PATH:" in *":$bindir:"*) ;; *) echo "aura install: add $bindir to PATH" ;; esac
