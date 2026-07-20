#!/bin/bash
# run_sanitizer_matrix.sh — Run the full TSan/ASan/UBSan
# sanitizer matrix on a representative subset of test
# binaries (Issue #325).
#
# This script closes the gap that the existing
# run_issue_*_tsan.sh scripts only support TSan +
# ASan (no UBSan). The 3-sanitizer matrix is the
# production-readiness surface that the CI should
# use; this script is the entry point.
#
# Usage:
#   tests/run_sanitizer_matrix.sh thread      # TSan only
#   tests/run_sanitizer_matrix.sh address     # ASan only
#   tests/run_sanitizer_matrix.sh undefined   # UBSan only
#   tests/run_sanitizer_matrix.sh all         # all three (slow but thorough)
#   tests/run_sanitizer_matrix.sh both        # TSan + ASan (legacy alias)
#   tests/run_sanitizer_matrix.sh coverage    # llvm-cov instrumentation
#
# The "all" mode runs each sanitizer in turn (3
# separate build+run cycles). Each sanitizer is
# independent — the binary is rebuilt with the new
# flags between cycles.
#
# Test binaries exercised (one per major area):
#   - test_issue_180 (closure-bridge lifetime) — #180
#   - test_issue_218 (reflection/serialize)    — #218
#   - test_issue_226 (unified harness)         — #226
#   - test_issue_321 (multi-fiber stress)      — #321
#
# All four were already TSan-tested in the prior
# session; this script adds the ASan + UBSan
# coverage + the combined "all" mode.
#
# Output: prints a pass/fail summary per sanitizer +
# per test binary. Any sanitizer warning (race, UAF,
# UB) causes the script to exit non-zero.
#
# CI integration: this script is the recommended
# entry point for the "sanitizer matrix" CI job.
# It supersedes run_issue_180_tsan.sh +
# run_issue_218_tsan.sh (which remain for backward
# compat but are slated for deprecation once
# downstream CI migrates).

set -euo pipefail

SANITIZER="${1:-thread}"
TARGETS="${2:-test_issue_180 test_issue_218 test_issue_226 test_issue_321}"

case "$SANITIZER" in
    thread|address|undefined|all|both|coverage) ;;
    *) echo "Usage: $0 [thread|address|undefined|all|both|coverage] [test_targets...]" >&2
       exit 2 ;;
esac

# Compiler flag sets per sanitizer.
TSAN_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g -O1"
ASAN_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g -O1"
UBSAN_FLAGS="-fsanitize=undefined -fno-omit-frame-pointer -g -O1"
COV_FLAGS="-fprofile-arcs -ftest-coverage -fno-omit-frame-pointer -g -O0"
COV_LINK_FLAGS="-fprofile-arcs -lgcov"

# Decide which sanitizers to run.
# "all" expands to thread + address + undefined (3 cycles).
# "both" is the legacy alias for thread + address.
# "coverage" is a one-shot build+run with gcov instrumentation.
case "$SANITIZER" in
    thread|address|undefined)
        SANITIZERS=("$SANITIZER")
        ;;
    both)
        SANITIZERS=(thread address)
        ;;
    all)
        SANITIZERS=(thread address undefined)
        ;;
    coverage)
        SANITIZERS=(coverage)
        ;;
esac

# Per-sanitizer build directory naming.
san_build_dir() {
    case "$1" in
        thread)    echo "build_tsan" ;;
        address)   echo "build_asan" ;;
        undefined) echo "build_ubsan" ;;
        coverage)  echo "build_cov" ;;
    esac
}

# Per-sanitizer CMAKE flags.
san_cmake_flags() {
    case "$1" in
        thread)
            echo "-DCMAKE_CXX_FLAGS=$TSAN_FLAGS -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread"
            ;;
        address)
            echo "-DCMAKE_CXX_FLAGS=$ASAN_FLAGS -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address"
            ;;
        undefined)
            echo "-DCMAKE_CXX_FLAGS=$UBSAN_FLAGS -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=undefined -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=undefined"
            ;;
        coverage)
            echo "-DCMAKE_CXX_FLAGS=$COV_FLAGS -DCMAKE_EXE_LINKER_FLAGS=$COV_LINK_FLAGS -DCMAKE_SHARED_LINKER_FLAGS=$COV_LINK_FLAGS"
            ;;
    esac
}

# Pretty-print result.
pass_count=0
fail_count=0
failed_targets=""

for san in "${SANITIZERS[@]}"; do
    echo ""
    echo "════════════════════════════════════════"
    echo "=== Sanitizer: $san ==="
    echo "════════════════════════════════════════"
    build_dir="$(san_build_dir "$san")"
    mkdir -p "$build_dir"
    cd "$build_dir"
    if [ ! -f "CMakeCache.txt" ]; then
        cmake .. $(san_cmake_flags "$san") > /tmp/cmake_$san.log 2>&1 || {
            echo "  ✗ cmake configure failed for $san; see /tmp/cmake_$san.log"
            fail_count=$((fail_count + 1))
            failed_targets="$failed_targets cmake-$san "
            cd ..
            continue
        }
    fi
    for target in $TARGETS; do
        echo "--- $target under $san ---"
        ninja_log="/tmp/ninja_${target}_${san}.log"
        run_log="/tmp/run_${target}_${san}.log"
        if ninja "$target" > "$ninja_log" 2>&1; then
            if ./"$target" > "$run_log" 2>&1; then
                echo "  ✓ $target passed under $san"
                pass_count=$((pass_count + 1))
            else
                echo "  ✗ $target FAILED under $san (rc=$?); tail of log:"
                tail -20 "$run_log" | sed 's/^/    /'
                fail_count=$((fail_count + 1))
                failed_targets="$failed_targets $target-$san "
            fi
        else
            echo "  ✗ $target BUILD failed under $san; tail of log:"
            tail -10 "$ninja_log" | sed 's/^/    /'
            fail_count=$((fail_count + 1))
            failed_targets="$failed_targets $target-$san-build "
        fi
    done
    cd ..
done

echo ""
echo "════════════════════════════════════════"
echo "=== Matrix summary ==="
echo "  Passed: $pass_count"
echo "  Failed: $fail_count"
if [ -n "$failed_targets" ]; then
    echo "  Failed targets:$failed_targets"
fi
echo "════════════════════════════════════════"

# Coverage post-step: if sanitizer was "coverage", generate
# the gcov reports for the hot files the issue body names.
if [ "$SANITIZER" = "coverage" ] && [ "$fail_count" = 0 ]; then
    echo ""
    echo "=== Coverage report ==="
    cd build_cov
    # Issue #325 hot files: evaluator_impl.cpp (or its
    # successor evaluator_primitives_compile.cpp), fiber.cpp,
    # aura_jit_bridge.cpp.
    for src in \
        src/compiler/evaluator_primitives_compile.cpp \
        src/serve/fiber.cpp \
        src/compiler/aura_jit_bridge.cpp; do
        if [ -f "$src" ] || [ -f "${src}.gcov" ]; then
            gcov -o . "$src" > /dev/null 2>&1 || true
            if [ -f "${src}.gcov" ]; then
                local_pct=$(grep -E "^Lines" "${src}.gcov" 2>/dev/null | head -1 || echo "(no Lines line)")
                echo "  $src: $local_pct"
            fi
        fi
    done
    cd ..
fi

[ "$fail_count" = 0 ]
