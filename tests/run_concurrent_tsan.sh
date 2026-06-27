#!/bin/bash
# run_concurrent_edsl_tsan.sh — Run test_issue_321
# under TSan to validate that the concurrent EDSL query-during-mutate
# path from Issue #332 holds without data races.
#
# This script rebuilds the test binary with -fsanitize=thread
# and runs it. The binary itself is the same; only the compiler
# instrumentation changes (TSan instruments every atomic + memory
# access to surface races that unit tests alone would miss).
#
# Usage:
#   tests/run_concurrent_edsl_tsan.sh
#   tests/run_concurrent_edsl_tsan.sh build_tsan_321   # custom build dir
#
# Output: prints pass/fail counts from the test binary. Any
# TSan warnings (data race on SharedState atomics or CompilerService
# internal state) cause the script to exit non-zero.
#
# CI integration: add this as a known-failure entry. The script
# is green once Issue #332 AC #5 (deadlock/starvation gate) is
# fully implemented; until then TSan warnings on `cs.eval()` and
# `cs.evaluator().workspace_flat()` are EXPECTED because the
# CompilerService isn't yet lock-free internally — we serialize
# at the test boundary via SharedState::eval_mtx.

set -e

BUILD_DIR="${1:-build_tsan_321}"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure with TSan flags. CMAKE_BUILD_TYPE=Debug is forced
# (TSan gives false positives at -O2/-O3). LLVM stays enabled
# because the test links aura_jit_runtime symbols.
TSAN_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g -O1"
cmake .. \
    -DCMAKE_CXX_FLAGS="$TSAN_FLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread \
    -DCMAKE_BUILD_TYPE=Debug \
    -DAURA_HAVE_LLVM=1

# Build just the test binary (faster than full rebuild).
ninja test_issue_321

# Run. TSan warnings go to stderr; we surface them.
echo "=== Running test_issue_321 under TSan ==="
./test_issue_321
