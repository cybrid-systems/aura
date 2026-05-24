#!/bin/bash
# Validate M4 borrow checker via --inspect typecheck
set +e  # don't exit on failure, we count them
AURA="./build/aura"
PASS=0
FAIL=0

check() {
    local name="$1" code="$2" expected="$3"
    local output=$(echo "$code" | $AURA --inspect typecheck 2>&1)
    if echo "$output" | grep -q "$expected"; then
        echo "  ✅ $name"
        ((PASS++))
    else
        echo "  ❌ $name (expected to contain: $expected)"
        ((FAIL++))
    fi
}

echo "=== M4 borrow checker validation ==="

check "borrow scope end → move ok" \
    '(let ((x (Linear 42))) (begin (let ((r (borrow x))) (display r)) (move x)))' \
    'typecheck result'

check "same scope borrow then move → error" \
    '(let ((x (Linear 42))) (let ((r (borrow x))) (move x) r))' \
    'cannot move x'

check "mut-borrow then borrow → error" \
    '(let ((x (Linear 42))) (let ((r (mut-borrow x))) (borrow x) r))' \
    'cannot borrow x'

check "multiple imm-borrow → ok" \
    '(let ((x (Linear 42))) (let ((r1 (borrow x))) (let ((r2 (borrow x))) (display r1) (display r2))) (move x))' \
    'typecheck result'

check "move then borrow → error" \
    '(let ((x (Linear 42))) (move x) (borrow x))' \
    'cannot borrow x'

check "move then drop → error" \
    '(let ((x (Linear 42))) (move x) (drop x))' \
    'cannot drop x'

check "double borrow (scope-separated) → ok" \
    '(let ((x (Linear 42))) (begin (let ((r (borrow x))) (display r)) (let ((r (borrow x))) (display r)) (move x)))' \
    'typecheck result'

echo ""
echo "Pass: $PASS  Fail: $FAIL"
exit $FAIL
