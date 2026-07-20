#!/bin/bash
# run_issue_218_tsan.sh — Run reflection tests under TSan/ASan
# (Issue #218 Cycle 5).
#
# Rebuilds test_issue_178 (1000+ serialize/deserialize stress)
# and test_issue_218 (integration) with -fsanitize=thread or
# -fsanitize=address, then runs both. Any sanitizer warning
# causes a non-zero exit.
#
# Usage:
#   tests/run_issue_218_tsan.sh thread   # TSan
#   tests/run_issue_218_tsan.sh address  # ASan
#   tests/run_issue_218_tsan.sh both     # both (slower)

set -euo pipefail

SANITIZER="${1:-thread}"

case "$SANITIZER" in
    thread|address|both) ;;
    *) echo "Usage: $0 [thread|address|both]" >&2; exit 2 ;;
esac

SAN_BUILD=build_tsan_218
mkdir -p "$SAN_BUILD"
cd "$SAN_BUILD"

TSAN_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g -O1"
ASAN_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g -O1"

# Per-sanitizer cmake flag arrays. The 'both' case combines TSAN_FLAGS +
# ASAN_FLAGS into a single -DCMAKE_CXX_FLAGS value (cmake takes the last
# flag, so the two flag sets MUST be merged into one string instead of
# passed as separate -D args).
case "$SANITIZER" in
    thread)
        CMAKE_FLAGS=(-DCMAKE_CXX_FLAGS="$TSAN_FLAGS" -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread)
        ;;
    address)
        CMAKE_FLAGS=(-DCMAKE_CXX_FLAGS="$ASAN_FLAGS" -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address)
        ;;
    both)
        CMAKE_FLAGS=(-DCMAKE_CXX_FLAGS="$TSAN_FLAGS $ASAN_FLAGS" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread -fsanitize=address")
        ;;
esac

cmake .. "${CMAKE_FLAGS[@]}"
ninja test_issue_178 test_issue_218

echo "=== Running test_issue_178 under $SANITIZER (1000+ stress) ==="
./test_issue_178

echo "=== Running test_issue_218 under $SANITIZER (integration) ==="
./test_issue_218