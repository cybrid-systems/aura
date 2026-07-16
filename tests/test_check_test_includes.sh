#!/usr/bin/env bash
# tests/test_check_test_includes.sh — Issue #1484 AC4 shell test
#
# Verifies that scripts/check_test_includes.py catches a fake bad include
# introduced in a scratch test file, with a clear error message that
# mentions both the broken include name and the required subdir prefix.
#
# This is the AC4 integration test: introduces a fake bad include in a
# scratch file under tests/, runs the linter, asserts:
#   - exit code == 1
#   - stderr contains the broken include name (e.g. "shape.h")
#   - stderr contains the required subdir prefix (e.g. "compiler")
#
# Cleans up the scratch file before exiting (so subsequent runs don't
# see the fake bad include in the linter's main pass).
#
# Exit 0 = all checks pass, 1 = any check failed.
#
# Run manually: bash tests/test_check_test_includes.sh
# Run from anywhere: bash /path/to/aura/tests/test_check_test_includes.sh

set -euo pipefail

# Resolve repo root (parent of tests/).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

LINTER="$REPO_ROOT/scripts/check_test_includes.py"
SCRATCH_DIR="$REPO_ROOT/tests"
SCRATCH_FILE="$SCRATCH_DIR/test_tmp_broken_include_1484.cpp"

# Defensive: ensure python3 is on PATH.
if ! command -v python3 >/dev/null 2>&1; then
    echo "FAIL: python3 not found on PATH" >&2
    exit 1
fi

# Defensive: ensure the linter exists.
if [ ! -f "$LINTER" ]; then
    echo "FAIL: linter not found at $LINTER" >&2
    exit 1
fi

# Cleanup helper: always run on exit (success or failure).
cleanup() {
    if [ -f "$SCRATCH_FILE" ]; then
        rm -f "$SCRATCH_FILE"
        echo "  cleanup: removed scratch file $SCRATCH_FILE"
    fi
}
trap cleanup EXIT

echo "AC4: tests/test_check_test_includes.sh — fake bad include detection"

# Step 1: introduce a fake bad include in a scratch file.
cat > "$SCRATCH_FILE" <<'EOF'
// Scratch test for Issue #1484 AC4 — should be caught by check_test_includes.py
// and cleaned up after the test. Do NOT commit this file.
#include "shape.h"

int main() {
    return 0;
}
EOF
echo "  step 1: created scratch file with bare '#include \"shape.h\"'"

# Step 2: run the linter — expect exit code 1.
set +e
python3 "$LINTER" > /tmp/check_includes_stdout_1484.log 2> /tmp/check_includes_stderr_1484.log
RC=$?
set -e

if [ "$RC" -ne 1 ]; then
    echo "FAIL: linter exit code expected 1, got $RC" >&2
    echo "  stdout:" >&2
    cat /tmp/check_includes_stdout_1484.log >&2
    echo "  stderr:" >&2
    cat /tmp/check_includes_stderr_1484.log >&2
    exit 1
fi
echo "  step 2: linter exited with code 1 (expected)"

# Step 3: verify stderr mentions the broken include name.
if ! grep -q 'shape\.h' /tmp/check_includes_stderr_1484.log; then
    echo "FAIL: stderr does not mention broken include name 'shape.h'" >&2
    echo "  stderr:" >&2
    cat /tmp/check_includes_stderr_1484.log >&2
    exit 1
fi
echo "  step 3: stderr mentions broken include name 'shape.h' (expected)"

# Step 4: verify stderr mentions the required subdir prefix.
if ! grep -q 'compiler' /tmp/check_includes_stderr_1484.log; then
    echo "FAIL: stderr does not mention required subdir 'compiler'" >&2
    echo "  stderr:" >&2
    cat /tmp/check_includes_stderr_1484.log >&2
    exit 1
fi
echo "  step 4: stderr mentions required subdir 'compiler' (expected)"

# Step 5: verify stdout is empty (linter writes to stderr on failure).
if [ -s /tmp/check_includes_stdout_1484.log ]; then
    echo "FAIL: stdout is non-empty (linter should write to stderr)" >&2
    echo "  stdout:" >&2
    cat /tmp/check_includes_stdout_1484.log >&2
    exit 1
fi
echo "  step 5: stdout is empty (linter writes to stderr)"

echo "OK: AC4 shell test passed — linter detects fake bad include with clear error"
exit 0
