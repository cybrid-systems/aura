#!/bin/bash
# Step 09 — Integer literal evaluation
# Each test returns 0 on pass, non-zero on fail
set -e

AURA="${1:-./aura}"

echo "=== Step 09: Integer Literal Evaluation ==="

# L1.1: positive integer literal
result=$(echo 42 | "$AURA" 2>/dev/null)
[ "$result" = "42" ] || { echo "FAIL: echo 42 | $AURA → expected 42, got $result"; exit 1; }
echo "  L1.1 PASS: literal 42"

# negative integer
result=$(echo -5 | "$AURA" 2>/dev/null)
[ "$result" = "-5" ] || { echo "FAIL: echo -5 → expected -5, got $result"; exit 1; }
echo "  L1.1 PASS: literal -5"

# large integer
result=$(echo 999999 | "$AURA" 2>/dev/null)
[ "$result" = "999999" ] || { echo "FAIL: echo 999999 → expected 999999, got $result"; exit 1; }
echo "  L1.1 PASS: literal 999999"

# variable → error
output=$(echo x | "$AURA" 2>&1)
echo "$output" | grep -q "unbound variable" || { echo "FAIL: echo x → expected 'unbound variable' error"; exit 1; }
echo "  L1.2 PASS: variable produces error"

echo "=== All Step 09 tests PASS ==="
