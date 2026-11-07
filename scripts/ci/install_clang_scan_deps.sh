#!/bin/bash
# Locate the clang-scan-deps that belongs to the clang++ this job runs.
#
# The version is the plain X.Y.Z in `clang++ --version`. A scanner that
# reports that same version is used as-is. Nothing is downloaded and the
# image copy is not replaced. Search PATH, the compiler's directory, and
# the usual LLVM prefix directories. If none match, fail: the aura-ci
# image is missing the tool, and the Clang module BMI build must not
# pretend otherwise.
set -euo pipefail

compiler=${CXX:-clang++}
if [[ "$compiler" != /* ]]; then
    compiler_path=$(command -v "$compiler" || true)
else
    compiler_path=$compiler
fi
if [[ -z "$compiler_path" || ! -x "$compiler_path" ]]; then
    echo "clang++ not found: $compiler" >&2
    exit 1
fi
compiler_real=$(readlink -f "$compiler_path")
compiler_dir=$(dirname "$compiler_real")

ver_line=$("$compiler_real" --version | head -1)
echo "compiler: $compiler_real" >&2
echo "compiler version: $ver_line" >&2

# "clang version X.Y.Z" then end, space, or "(". A git snapshot or
# X.Y.Z-rcN does not match.
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
    echo "refusing to select a scanner that may not match this clang++" >&2
    exit 1
fi
echo "matched clang++ LLVM version $ver" >&2
if [[ "${1:-}" == "--print-version" ]]; then
    echo "$ver"
    exit 0
fi

ver_re=${ver//./\\.}
major=${ver%%.*}
scanner_matches() {
    local bin=$1
    local out
    [[ -n "$bin" && -x "$bin" && ! -d "$bin" ]] || return 1
    out=$("$bin" --version 2>&1 || true)
    echo "clang-scan-deps ($bin): $out" >&2
    echo "$out" | grep -Eq "(LLVM|clang) version ${ver_re}([^0-9]|$)"
}

searched=()
mismatched=()
use_scanner() {
    local path=$1
    echo "using clang-scan-deps $ver at $path" >&2
    if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
        echo "path=$path" >> "$GITHUB_OUTPUT"
    fi
    if [[ -n "${GITHUB_ENV:-}" ]]; then
        echo "CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS=$path" >> "$GITHUB_ENV"
    fi
    printf '%s\n' "$path"
    exit 0
}
consider() {
    local path=$1
    searched+=("$path")
    [[ -x "$path" && ! -d "$path" ]] || return 0
    if scanner_matches "$path"; then
        use_scanner "$path"
    fi
    mismatched+=("$path")
}

consider "$compiler_dir/clang-scan-deps"
consider "$compiler_dir/clang-scan-deps-$major"
consider "$compiler_dir/clang-scan-deps-$ver"
if path_on_path=$(command -v clang-scan-deps 2>/dev/null || true); [[ -n "$path_on_path" ]]; then
    consider "$(readlink -f "$path_on_path")"
fi
consider "/usr/lib/llvm-$major/bin/clang-scan-deps"
consider "/usr/lib/llvm-$ver/bin/clang-scan-deps"
consider "/usr/lib/llvm/bin/clang-scan-deps"
consider "/usr/local/llvm/bin/clang-scan-deps"
consider "/usr/local/llvm-$ver/bin/clang-scan-deps"
consider "/opt/llvm/bin/clang-scan-deps"
consider "/opt/llvm-$ver/bin/clang-scan-deps"

echo "aura-ci image has no clang-scan-deps matching clang++ $ver" >&2
echo "compiler: $compiler_real" >&2
echo "compiler version: $ver_line" >&2
echo "searched:" >&2
for path in "${searched[@]}"; do
    echo "  $path" >&2
done
if [[ ${#mismatched[@]} -gt 0 ]]; then
    echo "found but version does not match $ver:" >&2
    for path in "${mismatched[@]}"; do
        echo "  $path" >&2
    done
fi
exit 1
