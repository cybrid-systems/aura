#!/usr/bin/env bash
# Issue #675: reproducible release tarball + SBOM.
#
# Usage:
#   SOURCE_DATE_EPOCH=1704067200 ./scripts/tools/release.sh 1.0.0
#
# Produces:
#   dist/aura-<version>.tar.gz
#   dist/aura-sbom.json
#   dist/aura-<version>-linux-<arch>.tar.gz (+ .sha256): the runnable bundle (#4907)
set -euo pipefail

# Issue #4907: this script lives in scripts/tools/, two levels below the root.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

VERSION="${1:-dev}"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1704067200}"
export AURA_VERSION="$VERSION"
export CCACHE_DISABLE=1
export AURA_BUILD_TYPE=Release

echo "═══ Aura release $VERSION (SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH) ═══"

python3 build.py repro build
# Issue #5203: produce the runnable bundle before the SBOM so the CycloneDX
# document can hash the artifact that actually ships (rt/ loader + libc).
scripts/tools/bundle.sh build_repro "$VERSION"
python3 build.py sbom --version "$VERSION"

STAGE="$ROOT/dist/stage-aura-$VERSION"
rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/share/aura"

cp build_repro/aura "$STAGE/bin/aura"
chmod 755 "$STAGE/bin/aura"
cp lib/runtime.c "$STAGE/lib/runtime.c"
cp lib/aura_value_abi.h "$STAGE/lib/aura_value_abi.h" # #4502: runtime.c includes it
cp -r demos "$STAGE/share/aura/demos" 2>/dev/null || true

TARBALL="$ROOT/dist/aura-$VERSION.tar.gz"
mkdir -p "$ROOT/dist"
tar -C "$STAGE/.." -czf "$TARBALL" "stage-aura-$VERSION"
rm -rf "$STAGE"


echo "✓ Release tarball: $TARBALL"
echo "✓ SBOM: $ROOT/dist/aura-sbom.json"
sha256sum "$TARBALL" "$ROOT/dist/aura-sbom.json" || true