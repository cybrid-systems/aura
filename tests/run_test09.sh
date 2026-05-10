#!/bin/bash
# Step 09 test runner
set -e

echo "=== Step 09: Integer Literal Eval ==="

# Test 1: positive integer
result=$(echo 42 | ./aura 2>/dev/null)
if [ "$result" != "42" ]; then
    echo "FAIL: expected 42, got $result"
    exit 1
fi
echo "PASS: literal 42"

# Test 2: negative integer
result=$(echo -5 | ./aura 2>/dev/null)
if [ "$result" != "-5" ]; then
    echo "FAIL: expected -5, got $result"
    exit 1
fi
echo "PASS: literal -5"

# Test 3: variable produces error
if echo x | ./aura 2>&1 | grep -q 'unbound variable'; then
    echo "PASS: variable error"
else
    echo "FAIL: expected unbound variable error"
    exit 1
fi

echo "=== All Step 09 tests PASS ==="
