#!/bin/bash
# run_issue_180_tsan.sh — Run test_issue_226 under TSan/ASan
# to validate the closure-bridge lifetime hardening from
# Issue #180 cycles 1-3 (Issue #226 cycle 4).
#
# This script rebuilds the test binary with -fsanitize=thread
# (or =address) and runs it. The binary itself is the same
# (test_issue_226.cpp); the sanitizer flags change how the
# compiler instruments the code, surfacing data races and
# use-after-frees that unit tests alone would miss.
#
# Usage:
#   tests/run_issue_180_tsan.sh thread   # TSan
#   tests/run_issue_180_tsan.sh address  # ASan
#   tests/run_issue_180_tsan.sh both     # both (slower, more thorough)
#
# Output: prints pass/fail counts from the test binary. Any
# TSan/ASan warnings (race, UAF, leak) cause the script to
# exit non-zero.

set -e

SANITIZER="${1:-thread}"

case "$SANITIZER" in
    thread|address|both) ;;
    *) echo "Usage: $0 [thread|address|both]" >&2; exit 2 ;;
esac

# Build dir for the sanitized binary
SAN_BUILD=build_tsan
mkdir -p "$SAN_BUILD"
cd "$SAN_BUILD"

# Configure CMake with the sanitizer flags
TSAN_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g -O1"
ASAN_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g -O1"

case "$SANITIZER" in
    thread)
        CMAKE_FLAGS="-DCMAKE_CXX_FLAGS=$TSAN_FLAGS -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread"
        ;;
    address)
        CMAKE_FLAGS="-DCMAKE_CXX_FLAGS=$ASAN_FLAGS -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address"
        ;;
    both)
        CMAKE_FLAGS="-DCMAKE_CXX_FLAGS=\"$TSAN_FLAGS $ASAN_FLAGS\" -DCMAKE_EXE_LINKER_FLAGS=\"-fsanitize=thread -fsanitize=address\""
        ;;
esac

cmake .. $CMAKE_FLAGS
ninja test_issue_226

# Run. TSan/ASan warnings go to stderr; we surface them.
echo "=== Running test_issue_226 under $SANITIZER ==="
./test_issue_226
