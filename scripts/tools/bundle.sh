#!/usr/bin/env bash
# Issue #4907: a portable aura bundle for this machine's architecture.
#
#   scripts/tools/bundle.sh [build-dir] [version]
#     -> dist/aura-<version>-linux-<arch>.tar.gz and .tar.gz.sha256
#
# The bundle carries its own loader, glibc and libstdc++ (rt/), so it runs on
# Linux hosts of the same architecture with an older glibc (CI verifies Ubuntu
# 24.04, glibc 2.39; the toolchain image is Ubuntu 26.10, glibc 2.44). bin/aura is a small launcher.
set -euo pipefail
ROOT=${AURA_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
BUILD=$(cd "${1:-$ROOT/build}" && pwd)
VERSION=${2:-$(git -C "$ROOT" rev-parse --short=9 HEAD)}
case "$(uname -m)" in
  x86_64 | amd64) ARCH=x86_64 ;;
  aarch64 | arm64) ARCH=arm64 ;;
  *) echo "bundle: unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac
NAME="aura-$VERSION-linux-$ARCH"
DIST=${DIST:-$ROOT/dist}
STAGE="$DIST/$NAME"
rm -rf "$STAGE" && mkdir -p "$STAGE/bin" "$STAGE/libexec" "$STAGE/rt"
cp "$BUILD/aura" "$STAGE/libexec/aura"
strip --strip-debug "$STAGE/libexec/aura" 2>/dev/null || true
cp -r "$ROOT/lib" "$STAGE/lib"
# Every shared library the binary resolves, then the loader itself.
ldd "$BUILD/aura" | awk '$2 == "=>" && $3 ~ /^\// { print $3 }' | while read -r so; do
  cp -L "$so" "$STAGE/rt/"
done
# Issue #5000: the arm64 CI image ships no patchelf. The interpreter path is
# already in the ELF program header, so read it with readelf (binutils, part of
# the toolchain) and keep patchelf only as a fallback.
INTERP=$(readelf -l "$BUILD/aura" 2>/dev/null \
  | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p' | head -1)
if [ -z "$INTERP" ] && command -v patchelf >/dev/null 2>&1; then
  INTERP=$(patchelf --print-interpreter "$BUILD/aura")
fi
if [ -z "$INTERP" ]; then
  echo "bundle: cannot determine the ELF interpreter of $BUILD/aura (need readelf or patchelf)" >&2
  exit 1
fi
cp -L "$INTERP" "$STAGE/rt/ld.so"
cat > "$STAGE/bin/aura" <<'EOF'
#!/bin/sh
# aura launcher (#4907): runs libexec/aura on the bundled loader and libraries
# in rt/, so the host glibc does not matter.
d=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
AURA_SELF=$d/bin/aura
AURA_BIN=${AURA_BIN:-$AURA_SELF}
AURA_PATH=${AURA_PATH:+$AURA_PATH:}$d/lib
AURA_RUNTIME_DIR=${AURA_RUNTIME_DIR:-$d}
export AURA_SELF AURA_BIN AURA_PATH AURA_RUNTIME_DIR
exec "$d/rt/ld.so" --library-path "$d/rt" "$d/libexec/aura" "$@"
EOF
chmod 755 "$STAGE/bin/aura"
{
  echo "aura $VERSION linux-$ARCH"
  echo "commit $(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "bundled glibc $(ldd --version | head -1 | awk '{print $NF}') (host glibc does not matter)"
  echo "host requirement: Linux kernel >= 3.2, $ARCH"
} > "$STAGE/BUILD-INFO"
tar -C "$DIST" -czf "$DIST/$NAME.tar.gz" "$NAME"
(cd "$DIST" && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
rm -rf "$STAGE"
echo "bundle: $DIST/$NAME.tar.gz ($(du -h "$DIST/$NAME.tar.gz" | cut -f1))"
