#!/bin/bash
# Aura module launcher (Fix #2: shared BMI cache, per-producer).
#
# GCC 16 writes .gcm files to <module_dir>/<name>.gcm, where module_dir
# is derived from the .o file's directory (the per-target dir like
# CMakeFiles/test_issue_130.dir/). This script, invoked by Ninja via
# CMAKE_CXX_COMPILER_LAUNCHER, runs g++ normally and then post-processes
# the per-target dir to:
#   1. Move any newly produced .gcm files into the shared cache dir
#      keyed by the producer (per_target_dir name), e.g.
#      $BUILD_DIR/module_cache/aura_test_objects.dir/<name>.gcm.
#      The producer key matters: aura and aura_test_objects compile the
#      same .ixx with different flags (AURA_HAVE_LLVM, AURA_HAVE_LIBGIT2,
#      extra -I paths), so their .gcm are NOT interchangeable — sharing
#      one across producers triggers a CRC mismatch on import.
#   2. Replace the per-target dir's .gcm with a symlink to the shared file
#      (under the same producer key).
#   3. Create the same symlink in every OTHER per-target dir that links
#      against the same static lib / uses the same compile flags. For the
#      aura project this means test_issue_* dirs that link against
#      aura_test_objects get the aura_test_objects producer's BMIs (the
#      ones their test_X.cpp imports expect).
#
# Concurrency: ninja runs many jobs in parallel. ln -sfn is idempotent,
# cp -f is atomic at the syscall level, and the source/cwd .gcm exists
# briefly during the move but the post-processing is fast. Worst case
# under heavy parallelism is a duplicate .gcm (a few hundred KB on
# disk), never a corrupt one.
set -u

SHARED_GCM_ROOT="${AURA_SHARED_GCM_DIR:-/home/dev/code/aura/build/module_cache}"
BUILD_DIR="${AURA_BUILD_DIR:-/home/dev/code/aura/build}"

# Extract the per-target dir from the -o path. e.g.
#   CMakeFiles/test_issue_130.dir/src/compiler/lowering.ixx.o
#   -> CMakeFiles/test_issue_130.dir
per_target_dir=""
args=("$@")
i=0
while [ $i -lt $# ]; do
    arg="${args[$i]}"
    if [ "$arg" = "-o" ]; then
        i=$((i+1))
        out_path="${args[$i]:-}"
        per_target_dir="$(printf '%s' "$out_path" | sed -E 's|((CMakeFiles/[^/]+\.dir))/.*|\1|')"
        break
    fi
    case "$arg" in
        -o*) per_target_dir="$(printf '%s' "${arg#-o}" | sed -E 's|((CMakeFiles/[^/]+\.dir))/.*|\1|')" ; break ;;
    esac
    i=$((i+1))
done

# GCC emits a noisy scan-phase warning when CMake's CXX module scanner
# invokes the compiler without linking. Filter it from stderr only.
/usr/bin/c++ "$@" 2> >(grep -v 'linker input file unused because linking not done' >&2)
RC=${PIPESTATUS[0]}

[ -n "$per_target_dir" ] || exit $RC
per_target_dir="$BUILD_DIR/$per_target_dir"
[ -d "$per_target_dir" ] || exit $RC

# Producer key = per-target dir basename (e.g. aura_test_objects.dir).
# The shared cache for this producer is module_cache/<producer-key>/.
producer_key="$(basename "$per_target_dir")"
PRODUCER_CACHE="$SHARED_GCM_ROOT/$producer_key"
mkdir -p "$PRODUCER_CACHE"

# Targets that should symlink to aura_test_objects's BMIs. Every
# test_issue_* (and friends) link against aura_test_objects and
# `import aura.compiler.X` in their test_X.cpp, so they need the
# .gcm aura_test_objects produced. Crucially: the .gcm is keyed by
# the producer's compile flags, and aura_test_objects's flags are
# the ones the test_issue_* consumers see when they look up
# `import` (their own compile flags match aura_test_objects's on
# the diagnostics that affect BMI content).
#
# Other producers (aura, test_ir, etc.) have different flags
# (e.g. -DAURA_HAVE_LLVM=1, -I.../compiler) and produce BMIs that
# would CRC-mismatch against the test_issue_* consumers. So those
# producers only symlink into their own per-target dir.
if [ "$producer_key" = "aura_test_objects.dir" ]; then
    # aura_test_objects is the canonical module producer for the
    # benchmark / unit-test binaries (test_*, issue_*, cycle*).
    # They all `import aura.compiler.X` (or aura.core.X) and rely
    # on the BMIs aura_test_objects produced. The launcher's symlinks
    # let their test_X.cpp find the BMIs at <their-cwd>/<name>.gcm.
    consumers=()
    # NOTE: bash doesn't expand globs inside variable values
    # (e.g. "$BUILD_DIR/CMakeFiles/$pat"*.dir keeps the `*` from
    # $pat as a literal). Use find with -name, which handles
    # wildcards in the pattern argument.
    while IFS= read -r d; do
        [ -d "$d" ] || continue
        consumers+=("$d")
    done < <(find "$BUILD_DIR/CMakeFiles" -maxdepth 1 \
                  \( -name 'test_*.dir' -o -name 'issue_*.dir' \
                     -o -name 'cycle_*.dir' \) -type d 2>/dev/null)
else
    # Other producers (aura, test_ir, etc.) have different flags
    # (e.g. -DAURA_HAVE_LLVM=1, -I.../compiler) and their BMIs
    # would CRC-mismatch against the test/issue/cycle consumers.
    # They only symlink into their own per-target dir.
    consumers=("$per_target_dir")
fi

sync_gcm() {
    local gcm_name="$1"
    local src="$per_target_dir/$gcm_name"
    local dst="$PRODUCER_CACHE/$gcm_name"

    if [ -L "$src" ]; then
        local target
        target="$(readlink -f "$src" 2>/dev/null || true)"
        # Only propagate when the symlink already points into OUR
        # producer cache. A symlink that points into a different
        # producer's cache (e.g. test_gc_evaluator_integration.dir/
        # pointing at test_ir.dir/) is a CROSS-PRODUCER pointer and
        # must be left alone — copying its target into our cache
        # would just bloat module_cache/<this-producer>/.
        if [ -n "$target" ] && \
           [ "${target%/*}" = "$PRODUCER_CACHE" ] && \
           [ -f "$target" ] && \
           { [ ! -f "$dst" ] || [ "$target" -nt "$dst" ]; }; then
            cp -f "$target" "$dst" 2>/dev/null || true
            # Touch the per-target symlinks in consumers so ninja's
            # mtime-based dep tracking notices the update.
            for c in "${consumers[@]}"; do
                [ -L "$c/$gcm_name" ] && touch -h "$c/$gcm_name" 2>/dev/null || true
            done
        fi
        return
    fi

    # Real file: copy to the producer's cache, then symlink in this
    # per-target dir AND in all consumer per-target dirs.
    if [ -f "$src" ]; then
        if [ ! -f "$dst" ] || [ "$src" -nt "$dst" ]; then
            cp -f "$src" "$dst" 2>/dev/null || true
        fi
        rm -f "$src"
        ln -sfn "$dst" "$src" 2>/dev/null || true
        for c in "${consumers[@]}"; do
            ln -sfn "$dst" "$c/$gcm_name" 2>/dev/null || true
        done
    fi
}

# Iterate .gcm files in the per-target dir (newly produced by
# this compile) AND in the producer's cache (so consumers added
# after the producer's last compile still get symlinks). The
# cache scan is the safety net: a .cpp compile in this target
# won't produce a .gcm itself, but it still propagates any
# prebuilt .gcm in PRODUCER_CACHE into the consumer per-target
# dirs. This is idempotent — ln -sfn overwrites the same
# symlink with the same target.
shopt -s nullglob
for gcm in "$per_target_dir"/*.gcm "$PRODUCER_CACHE"/*.gcm; do
    [ -e "$gcm" ] || continue
    sync_gcm "$(basename "$gcm")"
done
shopt -u nullglob

# After the per-file sync, ensure every consumer per-target dir
# has a symlink for every .gcm in PRODUCER_CACHE. The sync_gcm
# loop above only updates consumers when the cwd's .gcm is a
# REAL file (just produced); for a .cpp compile in the
# producer's dir, the cwd only has symlinks (from earlier
# compiles) and the sync is a no-op. This final pass fills
# the gap so a new consumer added after the last .ixx compile
# still gets its symlinks on the next .cpp build of the
# producer.
if [ -d "$PRODUCER_CACHE" ]; then
    for gcm_file in "$PRODUCER_CACHE"/*.gcm; do
        [ -e "$gcm_file" ] || continue
        name="$(basename "$gcm_file")"
        for c in "${consumers[@]}"; do
            # Skip the producer's own dir; that's already handled.
            [ "$c" = "$per_target_dir" ] && continue
            ln -sfn "$gcm_file" "$c/$name" 2>/dev/null || true
        done
    done
fi

exit $RC
