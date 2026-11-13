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
# and publishing a shared-cache .gcm is atomic: we write a same-directory
# temp file and rename(2) it into place (see publish_atomic below), so a
# concurrent reader never observes a truncated BMI. cp -f alone is NOT
# atomic — it truncates the destination in place, and a consumer that
# opens the symlinked .gcm mid-copy can mmap a zero-length module file,
# which GCC reports as EINVAL ('failed to read compiled module: Invalid
# argument'). Worst case under heavy parallelism is now a duplicate .gcm
# (a few hundred KB on disk), never a corrupt one.
set -euo pipefail

BUILD_DIR="${AURA_BUILD_DIR:-$PWD}"
SHARED_GCM_ROOT="${AURA_SHARED_GCM_DIR:-$BUILD_DIR/module_cache}"

# Publish a shared-cache .gcm atomically.
#
# cp -f truncates the destination in place and then writes it, so any
# reader that opens the destination (via a consumer's symlink) between
# truncate and write sees a zero-length / partial .gcm. GCC mmaps the
# module file and reports EINVAL ('failed to read compiled module:
# Invalid argument') for a 0-length BMI — exactly the CI build-test red.
#
# Instead, write to a same-directory temp file and rename(2) it into
# place. On the same filesystem rename is atomic: readers observe either
# the old complete file or the new complete file, never a partial one.
publish_atomic() {
    local from="$1" to="$2" tmp
    tmp="$to.tmp.$$"
    if cp -f "$from" "$tmp" 2>/dev/null && mv -f "$tmp" "$to" 2>/dev/null; then
        return 0
    fi
    rm -f "$tmp" 2>/dev/null || true
    return 1
}

# Atomically install (or refresh) a BMI symlink LINK -> TARGET.
#
# The per-target .gcm files are read by concurrent compiles, so the swap has
# to be atomic: removing the link and re-creating it leaves a window in which
# the module path does not exist at all, and a reader that opens it during
# that window dies with "failed to read compiled module" even though the BMI
# was produced. Create the link under a temp name and rename(2) it into
# place, so readers observe either the old complete link or the new complete
# link. Also refuse to link to a TARGET that does not exist yet: a dangling
# BMI symlink is worse than no link, because it turns a deterministic
# missing-module error into an intermittent read error.
link_bmi_atomic() {
    local target="$1" link="$2" tmp
    [ -e "$target" ] || return 1
    # Do not clobber a directory (mv would move into it instead of over it).
    [ -d "$link" ] && return 1
    tmp="$link.tmp.$$"
    if ln -sfn "$target" "$tmp" 2>/dev/null && mv -f "$tmp" "$link" 2>/dev/null; then
        return 0
    fi
    rm -f "$tmp" 2>/dev/null || true
    return 1
}

# Best-effort cleanup of any temp file left behind on failure/early exit.
cleanup_tmp_gcm() {
    rm -f "$SHARED_GCM_ROOT"/*/*.tmp."$$" 2>/dev/null || true
    # link_bmi_atomic temp links live beside the per-target .gcm files,
    # not under SHARED_GCM_ROOT, so clear those too.
    rm -f "$BUILD_DIR"/CMakeFiles/*/*.tmp."$$" 2>/dev/null || true
}
trap cleanup_tmp_gcm EXIT

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

if [ $# -lt 1 ]; then
    echo "aura_module_launcher.sh: missing compiler argument" >&2
    exit 127
fi

COMPILER="$1"
shift

# Optional ccache layer (Issue #873/#874).
# - CI sets CCACHE_DISABLE=1 → never wrap (avoids module BMI flakes).
# - Local: auto-wrap when ccache is on PATH and CCACHE_DISABLE is unset.
# - Explicit force-on: AURA_USE_CCACHE=1 / AURA_CCACHE=1 (build.py).
if [ -z "${CCACHE_DISABLE:-}" ] && command -v ccache >/dev/null 2>&1; then
    CXX=(ccache "$COMPILER")
else
    CXX=("$COMPILER")
fi

# GCC emits a noisy scan-phase warning when CMake's CXX module scanner
# invokes the compiler without linking. Filter it from stderr only.
#
# NOTE: process substitution is NOT a pipeline — do not use PIPESTATUS.
# Capture $? immediately after the compiler so a successful compile is
# not reported as FAILED (which left only post-process noise like
# "rm: cannot remove …gcm" visible on flaky CI logs).
set +e
"${CXX[@]}" "$@" 2> >(grep -v 'linker input file unused because linking not done' >&2 || true)
RC=$?
set -e

[ -n "$per_target_dir" ] || exit $RC
per_target_dir="$BUILD_DIR/$per_target_dir"
[ -d "$per_target_dir" ] || exit $RC

# Never post-process after a FAILED compile (RC != 0). The sync/fan-out
# below publishes BMIs into the shared cache and symlinks them into
# consumer per-target dirs; doing that after a failed compile would
# publish/link stale or partial artifacts. Bail out with the compiler's
# status, before any sync or fan-out.
[ "$RC" -eq 0 ] || exit "$RC"

# Issue #5202: only module-interface compiles produce BMIs. A plain .cpp
# compile has no new .gcm to publish, and its consumer dirs already got
# their symlinks from the producer's .ixx compile (target dirs are created
# at configure time, before any build). Skipping the cache scan + fan-out
# here removes ~0.8 s of readlink/stat per ordinary TU. Keep an escape
# hatch for a target added without a reconfigure.
case "$out_path" in
    *.ixx.o|*.cppm.o|*.bmi|*.gcm) ;;
    *) [ -n "${AURA_MODULE_LAUNCHER_ALWAYS_SYNC:-}" ] || exit "$RC" ;;
esac

# Producer key = per-target dir basename (e.g. aura_test_objects.dir).
# The shared cache for this producer is module_cache/<producer-key>/.
producer_key="$(basename "$per_target_dir")"
PRODUCER_CACHE="$SHARED_GCM_ROOT/$producer_key"
mkdir -p "$PRODUCER_CACHE"

# CMake 4.5 already generates a synthetic BMI target per module provider
# and its -fmodule-mapper points every consumer at that .bmi file
# (CMakeFiles/<producer>@synth_<n>.dir/<hash>.bmi). The old per-consumer
# .gcm symlink farm is therefore redundant: a test TU compiles with zero
# .gcm links (verified on #5202), while the 308-dir find + ~17k symlinks
# were pure build overhead. Keep only the producer's own dir link.
consumers=("$per_target_dir")

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
            publish_atomic "$target" "$dst" || true
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
            publish_atomic "$src" "$dst" || true
        fi
        # Publish-then-link, never delete-then-link. If the shared-cache
        # copy is not present (publish failed - e.g. a sibling launcher in
        # this same dir removed this real .gcm between our -f check and the
        # cp), keep the real BMI in place. The old sequence
        #   rm -f "$src"; ln -sfn "$dst" "$src"
        # deleted the only complete copy and replaced it with a DANGLING
        # symlink, so the next reader (a sibling compile in this dir, or a
        # consumer import) died with "failed to read compiled module" while
        # a retry - once the cache copy existed - succeeded.
        if [ -f "$dst" ]; then
            # Never fail the compile on a concurrent gcm restat race.
            link_bmi_atomic "$dst" "$src" || true
            for c in "${consumers[@]}"; do
                # Fast path: a live link (or file) already resolves. The
                # shared cache path is stable, so a republish does not
                # need a fresh ln/mv pair per consumer.
                [ -e "$c/$gcm_name" ] && continue
                link_bmi_atomic "$dst" "$c/$gcm_name" || true
            done
        fi
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
# Issue #5202: the per-target dir and the producer cache normally hold the
# same module names; the old loop ran sync_gcm (and its readlink -f) twice
# per module. Keep one pass per module name.
declare -A _aura_seen_gcm=()
for gcm in "$per_target_dir"/*.gcm "$PRODUCER_CACHE"/*.gcm; do
    [ -e "$gcm" ] || continue
    _aura_gcm_name="$(basename "$gcm")"
    [ -n "${_aura_seen_gcm[$_aura_gcm_name]:-}" ] && continue
    _aura_seen_gcm["$_aura_gcm_name"]=1
    sync_gcm "$_aura_gcm_name"
done
unset _aura_seen_gcm _aura_gcm_name
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
            # Issue #5202: this safety net runs on every compile. A live
            # link already points at PRODUCER_CACHE/$name (the path is
            # stable across republish), so skip the ln/mv pair; only
            # fill gaps for consumers added after the producer compiled.
            # 308 consumers × 53 BMIs of ln+mv forks dominated clean
            # test builds (~20 min); an existence test is a stat(2).
            [ -e "$c/$name" ] && continue
            link_bmi_atomic "$gcm_file" "$c/$name" || true
        done
    done
fi

exit $RC
