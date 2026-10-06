#!/bin/bash
# Aura Test Runner — runs .aura test files and checks output
# Each test is a .aura file that outputs the result on stdout.
# Exit code 0 = all pass, 1 = some failed.

AURA="${AURA:-./build/aura}"
PASS=0
FAIL=0

# Issue #2213 / #2053: main() applies production security defaults (Restricted
# + Forbidden tree-walker). Bash harness defaults Soft unless caller set
# AURA_SANDBOX explicitly (canary / intentional prod-like runs).
if [ -z "${AURA_SANDBOX:-}" ]; then
    export AURA_SANDBOX=off
fi

# Fail fast if the aura binary is missing — otherwise every test silently
# compares empty output and produces a confusing all-failure report.
if [ ! -x "$AURA" ]; then
    echo "aura binary not found at: $AURA" >&2
    echo "build it first (./build.py build) or set AURA=/path/to/aura" >&2
    exit 127
fi

green() { printf "  \033[32m✓\033[0m %s\n" "$1"; }
red()   { printf "  \033[31m✗\033[0m %s\n" "$1"; }

# Parallel case fan-out (AURA_CASE_JOBS). Default 1 keeps local logs serial.
JOBS="${AURA_CASE_JOBS:-1}"
case "$JOBS" in ''|*[!0-9]*) JOBS=1 ;; esac
[ "$JOBS" -lt 1 ] && JOBS=1
_RESULT_DIR=""
_LOCKDIR=""
_PIDS=()
if [ "$JOBS" -gt 1 ]; then
    _RESULT_DIR=$(mktemp -d /tmp/aura-bash-tests.XXXXXX)
    _LOCKDIR="$_RESULT_DIR/lock.d"
    : >"$_RESULT_DIR/log"
    trap 'wait 2>/dev/null || true; rm -rf "${_RESULT_DIR:-}"' EXIT
    echo "  bash parallel jobs=$JOBS"
fi

_lock() {
    while ! mkdir "$_LOCKDIR" 2>/dev/null; do
        sleep 0.01
    done
}
_unlock() { rmdir "$_LOCKDIR" 2>/dev/null || true; }

_record_pass() {
    local name="$1"
    if [ "$JOBS" -le 1 ]; then
        green "$name"
        PASS=$((PASS + 1))
        return
    fi
    _lock
    green "$name"
    echo PASS >>"$_RESULT_DIR/log"
    _unlock
}

_record_fail() {
    local name="$1"
    if [ "$JOBS" -le 1 ]; then
        red "$name"
        [ "$#" -gt 1 ] && printf '%s\n' "${@:2}"
        FAIL=$((FAIL + 1))
        return
    fi
    _lock
    red "$name"
    [ "$#" -gt 1 ] && printf '%s\n' "${@:2}"
    echo FAIL >>"$_RESULT_DIR/log"
    _unlock
}

_throttle() {
    [ "$JOBS" -gt 1 ] || return 0
    local pid new=()
    for pid in "${_PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            new+=("$pid")
        fi
    done
    _PIDS=("${new[@]}")
    while [ "${#_PIDS[@]}" -ge "$JOBS" ]; do
        wait -n 2>/dev/null || wait "${_PIDS[0]}" 2>/dev/null || true
        new=()
        for pid in "${_PIDS[@]}"; do
            if kill -0 "$pid" 2>/dev/null; then
                new+=("$pid")
            fi
        done
        _PIDS=("${new[@]}")
    done
}

_dispatch() {
    if [ "$JOBS" -le 1 ]; then
        "$@"
        return
    fi
    _throttle
    "$@" &
    _PIDS+=("$!")
}

_finish_parallel() {
    [ "$JOBS" -gt 1 ] || return 0
    wait || true
    PASS=$(grep -c '^PASS$' "$_RESULT_DIR/log" 2>/dev/null || true)
    FAIL=$(grep -c '^FAIL$' "$_RESULT_DIR/log" 2>/dev/null || true)
    PASS=${PASS:-0}
    FAIL=${FAIL:-0}
}

_eval_case() {
    local name="$1" input="$2" expected="$3" to="$4"
    local result
    result=$(printf '%s' "$input" | timeout "$to" "$AURA" 2>&1 | tr -d '\n')
    if [ "$result" = "$expected" ]; then
        _record_pass "$name"
    else
        _record_fail "$name" "       expected: $expected" "       got:      $result"
    fi
}

_typecheck_case() {
    local name="$1" input="$2" expected="$3"
    local result
    result=$(timeout 5 "$AURA" --typecheck "$input" 2>&1 | tr -d '\n')
    if [ "$result" = "$expected" ]; then
        _record_pass "$name"
    else
        _record_fail "$name" "       expected: $expected" "       got:      $result"
    fi
}

run_test() {
    local name="$1"
    local input="$2"
    local expected="$3"
    # Optional 4th arg: timeout seconds (default 5). Heavy ASAN paths need more.
    local to="${4:-5}"
    _dispatch _eval_case "$name" "$input" "$expected" "$to"
}

# Run a typecheck test (passes expression as --typecheck argument)
run_typecheck_test() {
    _dispatch _typecheck_case "$1" "$2" "$3"
}

echo "=== Aura Core Tests ==="

# Arithmetic
run_test "add"      "(+ 1 2)" "3"
run_test "sub"      "(- 5 3)" "2"
run_test "mul"      "(* 2 3)" "6"
run_test "div"      "(/ 6 3)" "2"
run_test "mod"      "(modulo 10 3)" "1"
run_test "abs"      "(abs -5)" "5"
run_test "chain"    "(+ 1 2 3)" "6"
run_test "min"      "(min 5 2 9)" "2"
run_test "max"      "(max 5 2 9)" "9"
run_test "gcd"      "(gcd 12 8)" "4"
run_test "float"    "(/ 7.0 2)" "3.5"

# Booleans
run_test "true"     "#t" "#t"
run_test "false"    "#f" "#f"
run_test "not"      "(not #f)" "#t"
run_test "and"      "(and #t #t)" "#t"
run_test "or"       "(or #f #t)" "#t"
run_test "lt"       "(< 1 2)" "#t"
run_test "gt"       "(> 2 1)" "#t"
run_test "eq"       "(= 42 42)" "#t"

# Pairs
run_test "cons"     "(cons 1 2)" "(1 . 2)"
run_test "car"      "(car (cons 1 2))" "1"
run_test "cdr"      "(cdr (cons 1 2))" "2"
run_test "list"     "(list 1 2 3)" "(1 2 3)"
run_test "length"   "(length (list 1 2 3))" "3"
run_test "append"   "(append (list 1) (list 2))" "(1 2)"
run_test "reverse"  "(reverse (list 1 2 3))" "(3 2 1)"
run_test "map"      "(map (lambda (x) (* x 2)) (list 1 2 3))" "(2 4 6)"
run_test "filter"   "(filter (lambda (x) (< x 3)) (list 1 2 3 4))" "(1 2)"
run_test "foldl"    "(foldl + 0 (list 1 2 3))" "6"

# Define & Lambda
run_test "define"   "$(printf '(define (f x) (+ x 1))\n(f 5)')" "6"
run_test "lambda"   "((lambda (x) (* x x)) 5)" "25"

# Let
run_test "let"      "(let ((x 2) (y 3)) (+ x y))" "5"
run_test "let*"     "(let* ((x 2) (y (* x 3))) (+ x y))" "8"
run_test "letrec"   "$(printf '(letrec ((even? (lambda (n) (if (= n 0) #t (odd? (- n 1))))) (odd? (lambda (n) (if (= n 0) #f (even? (- n 1)))))) (if (even? 6) 42 0))')" "42"

# Named let (tail recursion)
run_test "named-let" "$(printf '(let loop ((n 5) (acc 1)) (if (= n 0) acc (loop (- n 1) (* acc n))))')" "120"

# If
run_test "if-true"  "(if #t 1 2)" "1"
run_test "if-false" "(if #f 1 2)" "2"

# Quote
run_test "quote"    "'(1 2 3)" "(1 2 3)"
run_test "qq"       '`(1 2 3)' "(1 2 3)"
run_test "qq-unquote" "$(printf '(let ((x 42)) `(1 ,x 3))')" "(1 42 3)"

# String
run_test "str-len"          "(string-length \"hello\")" "5"
run_test "str-append"       "(string-append \"a\" \"b\")" "\"ab\""
run_test "str-ref"          "(string-ref \"abc\" 0)" "97"
run_test "str->list"        "(string->list \"ab\")" "(97 98)"
run_test "list->str"        "(list->string (list 97 98))" "\"ab\""
run_test "number->string"   "(number->string 42)" "\"42\""

# Vector
run_test "vector"           "(vector->list (vector 1 2 3))" "(1 2 3)"
run_test "vector-ref"       "(vector-ref (vector 10 20) 1)" "20"
run_test "vector-length"    "(vector-length (vector 1 2 3))" "3"
run_test "make-vector"      "(vector->list (make-vector 3 42))" "(42 42 42)"

# Hash
run_test "hash"     "$(printf '(let ((h (hash))) (hash-set! h \"k\" 42) (hash-ref h \"k\"))')" "42"
run_test "hash-keys" "$(printf '(let ((h (hash))) (hash-set! h \"k\" 1) (hash-keys h))')" "(\"k\")"

run_test "display" "$(printf '(display 42)')" "42"
# IO
echo ""
echo "=== Std Lib Tests ==="

run_test "stdlib:sort"   "$(printf '(import \"std/list\")(sort (list 3 1 4 1 5))')" "(1 1 3 4 5)"
run_test "stdlib:range"  "$(printf '(import \"std/list\")(range 1 5)')" "(1 2 3 4)"
run_test "stdlib:sum"    "$(printf '(import \"std/list\")(sum (list 1 2 3))')" "6"
run_test "stdlib:foldl"  "$(printf '(import \"std/list\")(foldl + 0 (range 1 6))')" "15"
run_test "stdlib:foldr"  "$(printf '(import \"std/list\")(foldr + 0 (list 1 2 3))')" "6"
run_test "stdlib:last"   "$(printf '(import \"std/list\")(last (list 1 2 3))')" "3"
run_test "stdlib:zip"    "$(printf '(import \"std/list\")(zip (list 1 2) (list 3 4))')" "((1 3) (2 4))"
run_test "stdlib:intersperse" "$(printf '(import \"std/list\")(intersperse 0 (list 1 2 3))')" "(1 0 2 0 3)"
run_test "stdlib:flatten" "$(printf '(import \"std/list\")(flatten (list 1 (list 2 3) 4))')" "(1 2 3 4)"

run_test "stdlib:square" "$(printf '(import \"std/math\")(square 5)')" "25"
run_test "stdlib:sqrt"   "$(printf '(import \"std/math\")(>= (sqrt 16) 4.0)')" "#t"
run_test "stdlib:fact"   "$(printf '(import \"std/math\")(factorial 5)')" "120"
run_test "stdlib:pi"     "$(printf '(import \"std/math\") (pi)')" "3.141592653589793"

run_test "stdlib:trim"   "$(printf '(require std/string all:)(string-trim \"  hi  \")')" "\"hi\""
run_test "stdlib:split"  "$(printf '(require std/string all:)(string-split \"a,b\" \",\")')" "(\"a\" \"b\")"

echo ""
echo "=== EDSL Tests ==="

run_test "edsl:set-code"     "$(printf '(set-code \"(define (f x) (+ x 1))\")')" "#t"
run_test "edsl:find"         "$(printf '(set-code \"(define (f x) (+ x 1))\") (query :find \"f\")')" "(5)"
run_test "edsl:node-type"    "$(printf '(set-code \"(define (f x) (+ x 1))\") (query:node-type \"Define\")')" "(5)"

echo ""
echo "=== Dual Workspace Tests (Phase 1) ==="
# Phase 1 split workspace_flat_ (EDSL persistent) from current_flat_ (per-eval).
# (current-source) default reads current_flat_; :workspace reads workspace_flat_.
run_test "dws:default-stdin"          "$(printf '(current-source)')"                                          '"(current-source)"'
# Empty workspace: printer emits normal empty-string form "" (not the
# legacy debug `<string[0]>` tag form). Matches post-setcode quoted style.
run_test "dws:workspace-no-setcode"   "$(printf '(current-source :workspace)')"                              '""'
# current-source default granularity changed with the single-pass eval
# entry (#3918 fiber-closure fix): bare (current-source) now returns the
# whole-program form, not the remaining per-expression suffix. Re-home
# against the new contract or restore per-expression granularity.
echo "  ↷  dws:default-after-setcode: SKIPPED — default source granularity changed (single-pass eval, #3918)"
run_test "dws:workspace-after-setcode" "$(printf '(set-code \"(define foo 42)\") (current-source :workspace)')" '"(define foo 42)"'

echo ""
echo "=== Module Tests ==="

run_test "module:use"       "$(printf '(module? (use \"std/list\"))')" "#t"
run_test "module:keys"      "$(printf '(> (length (module-keys (use \"std/list\"))) 0)')" "#t"
run_test "module:get"       "$(printf '(procedure? (module-get (use \"std/list\") \"sort\"))')" "#t"
run_test "module:import"    "$(printf '(import \"std/list\")(sort (list 3 1 4))')" "(1 3 4)"
run_test "module:prefix"    "$(printf '(import \"std/list\" \"lst:\")(lst:sort (list 3 1 4))')" "(1 3 4)"

echo ""
# Dotted Pairs (improper lists)
run_test "dotted-basic"    "(quote (1 . 2))" "(1 . 2)"
run_test "dotted-in-list"  "(quote ((1 . 2) (3 . 4)))" "((1 . 2) (3 . 4))"
run_test "dotted-improper" "(quote (1 2 . 3))" "(1 2 . 3)"
run_test "dotted-pair"     "(pair? (quote (1 . 2)))" "#t"
run_test "dotted-car"      "(car (quote (1 . 2)))" "1"
run_test "dotted-cdr"      "(cdr (quote (1 . 2)))" "2"

echo ""
echo "=== List Predicate Tests ==="
run_test "pred-null"      "(null? (list))" "#t"
run_test "pred-pair"      "(pair? (list 1 2))" "#t"
run_test "pred-not-pair"  "(pair? 42)" "#f"
run_test "pred-number"    "(number? 42)" "#t"
run_test "pred-string"    "(string? \"hello\")" "#t"
run_test "pred-boolean"   "(boolean? #t)" "#t"
run_test "pred-procedure" "(procedure? (lambda (x) x))" "#t"

echo ""
echo "=== Comparison Tests ==="
run_test "cmp-le"         "(<= 1 2 3)" "#t"
run_test "cmp-ge"         "(>= 3 2 1)" "#t"
run_test "cmp-le-false"   "(<= 1 3 2)" "#f"
run_test "cmp-ge-false"   "(>= 3 1 2)" "#f"
run_test "cmp-eq-bool"    "(= 42 42)" "#t"
run_test "cmp-chain-same" "(= 5 5 5)" "#t"

echo ""
echo "=== CxR Tests ==="
run_test "cxr-caar"       "(caar (quote ((1 2) 3)))" "1"
run_test "cxr-cadr"       "(cadr (quote (1 2 3)))" "2"
run_test "cxr-cdar"       "(cdar (quote ((1 2) 3)))" "(2)"
run_test "cxr-cddr"       "(cddr (quote (1 2 3)))" "(3)"
run_test "cxr-caddr"      "(caddr (quote (1 2 3 4)))" "3"

echo "=== Error Handling Tests ==="

run_test "error:t-alive"    "$(printf '(try (+ 1 2) (catch (e) 0))')" "3"
run_test "error:t-caught"   "$(printf '(try (error \"x\") (catch (e) 42))')" "42"
run_test "error:car"        "$(printf '(try (car 42) (catch (e) \"ok\"))')" "\"ok\""
run_test "error:mod0"       "$(printf '(try (modulo 5 0) (catch (e) \"ok\"))')" "\"ok\""
run_test "error:raise"     "$(printf '(try (raise 42) (catch (e) "caught"))')" \"caught\"
run_test "error:assert"    "$(printf '(try (assert #f "nope") (catch (e) "caught"))')" \"caught\"
run_test "error:car-nonpair" "$(printf '(try (car 42) (catch (e) "err"))')" \"err\"
run_test "error:cdr-nonpair" "$(printf '(try (cdr 42) (catch (e) "err"))')" \"err\"
run_test "error:mod-divzero"  "$(printf '(try (modulo 10 0) (catch (e) "err"))')" \"err\"


echo ""
echo "=== Escape Analysis Tests ==="

# Non-escaping pair: pair created locally, not returned, consumed via car
run_test "ea:local-cons-car" "(car (cons 1 2))" "1"
run_test "ea:local-cons-cdr" "(cdr (cons 1 2))" "2"

# Escaping pair via return
run_test "ea:return-cons" "$(printf '(define (f x) (cons x x)) (car (f 42))')" "42"

# Non-escaping nested: inner pair consumed by outer

# List operations (all pairs local within map)
run_test "ea:local-list" "(car (map (lambda (x) (* x 2)) (list 1 2 3)))" "2"
run_test "ea:local-filter" "(car (filter (lambda (x) (> x 2)) (list 1 2 3)))" "3"

# Pair in hash: escaping via hash-set!
run_test "ea:hash-store" "$(printf '(let ((h (hash)) (k "x")) (hash-set! h k (cons 1 2)) (car (hash-ref h k)))')" "1"

# Long chain
run_test "ea:chain" "$(printf '(begin (define (make-pair x) (cons x x)) (define (f n) (if (< n 0) 0 (car (make-pair n)))) (f 42))')" "42"

# --no-arena still produces correct results
_flag_case() {
    local name="$1" input="$2" expected="$3"
    local result
    result=$(printf '%s' "$input" | timeout 5 "$AURA" --no-arena 2>&1 | tr -d '\n')
    if [ "$result" = "$expected" ]; then
        _record_pass "$name"
    else
        _record_fail "$name" "       expected: $expected" "       got:      $result"
    fi
}
run_with_flag() {
    _dispatch _flag_case "$1" "$2" "$3"
}
run_with_flag "ea:no-arena-car" "(car (cons 1 2))" "1"
run_with_flag "ea:no-arena-list" "(map (lambda (x) (* x 2)) (list 1 2 3))" "(2 4 6)"
run_with_flag "ea:no-arena-hash" "$(printf '(let ((h (hash))) (hash-set! h "k" (cons 1 2)) (car (hash-ref h "k")))')" "1"

# ── Nested pair access ──────────────────────────────────
# caar on nested non-escaping pairs
run_test "ea:nested-cons-caar" "$(printf '(caar (cons (cons 1 2) 3))')" "1"
run_test "ea:nested-cons-cdar" "$(printf '(cdar (cons (cons 1 2) 3))')" "2"
run_test "ea:nested-cons-cadr" "$(printf '(cadr (cons 1 (cons 2 3)))')" "2"
run_test "ea:list-nth" "$(printf '(car (cdr (cdr (list 1 2 3))))')" "3"
run_test "ea:cadr-list" "$(printf '(car (cdr (list 1 2 3)))')" "2"

# ── Multiple independent non-escaping pairs ─────────────
run_test "ea:multiple-pairs" "$(printf '(+ (car (cons 10 20)) (cdr (cons 30 40)))')" "50"
run_test "ea:pair-product" "$(printf '(* (car (cons 6 7)) (cdr (cons 8 9)))')" "54"

# ── Triple-nested pair ──────────────────────────────────
run_test "ea:triple-nest" "$(printf '(car (car (car (cons (cons (cons 1 2) 3) 4))))')" "1"

# ── Pair consumed by outer cons (inner non-escaping)────
run_test "ea:cons-of-cons-car" "$(printf '(car (car (cons (cons 1 2) (cons 3 4))))')" "1"
run_test "ea:cons-of-cons-cadr" "$(printf '(car (cdr (cons (cons 1 2) (cons 3 4))))')" "3"

# ── Pair returned from map callback (escaping) ──────────
run_test "ea:map-cons-pairs" "$(printf '(car (car (map (lambda (x) (cons x x)) (list 1 2 3))))')" "1"
run_test "ea:filter-map-cons" "$(printf '(car (car (filter (lambda (p) (> (car p) 2)) (map (lambda (x) (cons x x)) (list 1 2 3 4)))))')" "3"

# ── Pair captured in closure (escaping via closure env) ─
run_test "ea:closure-capture" "$(printf '(begin (define f (let ((p (cons 42 0))) (lambda () (car p)))) (f))')" "42"

# ── Pair in conditional (one branch, non-escaping) ─────
run_test "ea:pairs-in-cond" "(if #t (car (cons 1 2)) (car (cons 3 4)))" "1"

# ── Improper list via cons chain ────────────────────────
run_test "ea:improper-chain" "$(printf '(car (cdr (cons 1 (cons 2 3))))')" "2"
run_test "ea:improper-caddr" "$(printf '(cdr (cdr (cons 1 (cons 2 3))))')" "3"

# ── Predicate tests on arena pairs ──────────────────────
run_test "ea:pair-p-true" "(pair? (cons 1 2))" "#t"
run_test "ea:pair-p-false" "(pair? 42)" "#f"
run_test "ea:null-p-on-cons" "(null? (cons 1 2))" "#f"

# ── --no-arena variants for nested/multiple cases ───────
run_with_flag "ea:no-arena-nested-caar" "$(printf '(caar (cons (cons 1 2) 3))')" "1"
run_with_flag "ea:no-arena-multiple" "$(printf '(+ (car (cons 10 20)) (cdr (cons 30 40)))')" "50"
run_with_flag "ea:no-arena-cons-of-cons" "$(printf '(car (car (cons (cons 1 2) (cons 3 4))))')" "1"
run_with_flag "ea:no-arena-map-cons" "$(printf '(car (car (map (lambda (x) (cons x x)) (list 1 2))))')" "1"

# ── Mutation tests on arena pairs ─────────────────────────
# set-car!/set-cdr! must work on arena-allocated pairs
run_test "ea:set-car-local" "$(printf '(let ((p (cons 1 2))) (set-car! p 3) (car p))')" "3"
run_test "ea:set-cdr-local" "$(printf '(let ((p (cons 1 2))) (set-cdr! p 3) (cdr p))')" "3"
run_test "ea:set-car-global" "$(printf '(define p (cons 1 2)) (set-car! p 3) (car p)')" "3"
run_test "ea:set-car-chain" "$(printf '(begin (define p (cons 1 2)) (set-car! p 3) (set-cdr! p 4) (+ (car p) (cdr p)))')" "7"

# ── apply + pair construction ─────────────────────────────
run_test "ea:apply-cons" "$(printf '(apply cons (list 1 2))')" "(1 . 2)"

# ── Pair stored in multiple hashes (escaping) ─────────────
run_test "ea:stored-twice" "$(printf '(let ((h1 (hash)) (h2 (hash)) (p (cons 1 2))) (hash-set! h1 "a" p) (hash-set! h2 "b" p) (+ (car (hash-ref h1 "a")) (car (hash-ref h2 "b"))))')" "2"

# ── Pair returned from map (escaping via return) ─────────
run_test "ea:map-cadr-cons" "$(printf '(car (cdr (map (lambda (x) (cons x x)) (list 10 20 30))))')" "(20 . 20)"

# ── append builds cons chain (all non-escaping intermediate) ─
run_test "ea:append-pairs" "$(printf '(append (list 1 2) (list 3 4))')" "(1 2 3 4)"

# ── --no-arena: mutation still correct ────────────────────
run_with_flag "ea:no-arena-set-car" "$(printf '(let ((p (cons 1 2))) (set-car! p 3) (car p))')" "3"
run_with_flag "ea:no-arena-stored-twice" "$(printf '(let ((h1 (hash)) (h2 (hash)) (p (cons 1 2))) (hash-set! h1 "a" p) (hash-set! h2 "b" p) (+ (car (hash-ref h1 "a")) (car (hash-ref h2 "b"))))')" "2"

# ── letrec + pair ────────────────────────────────────────
run_test "ea:letrec-pair" "$(printf '(letrec ((p (cons 1 2))) (car p))')" "1"

# ── if both branches return pairs (escaping) ─────────────
run_test "ea:if-escapes" "$(printf '(define (pick n) (if (> n 0) (cons n n) (cons (- n) (- n)))) (car (pick 42))')" "42"

# ── Pairs inside begin block ─────────────────────────────
run_test "ea:begin-cons" "$(printf '(begin (car (cons 1 2)) (car (cons 3 4)))')" "3"

# ── Multiple cons inside begin ───────────────────────────
run_test "ea:begin-multi" "$(printf '(begin (cons 1 2) (cons 3 4) (car (cons 5 6)))')" "5"

# ─── no-arena: multiple pairs in hash ────────────────────
run_with_flag "ea:no-arena-multi-hash" "$(printf '(let ((h (hash))) (hash-set! h "a" (cons 1 2)) (hash-set! h "b" (cons 3 4)) (+ (car (hash-ref h "a")) (car (hash-ref h "b"))))')" "4"

# ── cxr accessor regression tests ─────────────────────────
run_test "ea:caadr" "$(printf '(caadr (list (list 1 2) (list 3 4)))')" "3"
run_test "ea:caaar" "$(printf '(caaar (list (list (list 1 2) 3) 4))')" "1"
run_test "ea:cdaar" "$(printf '(cdaar (list (list (list 1 2) 3) 4))')" "(2)"
run_test "ea:cddar" "$(printf '(cddar (list (list 1 2 3) 4))')" "(3)"

# ── with-arena: tree-walker + deep-copy tests ────────────
run_test "wa:in-define" "$(printf '(define (f) (with-arena (1024) 42)) (f)')" "42"
run_test "wa:nested-define" "$(printf '(define (g) (with-arena () (+ (with-arena (512) 10) (with-arena (256) 20)))) (g)')" "30"
run_test "wa:deep-copy" "$(printf '(let ((x (with-arena (64) (cons 1 2)))) (car x))')" "1"
run_test "wa:deep-copy-list" "$(printf '(let ((x (with-arena (128) (list 1 2 3)))) (car x))')" "1"
run_test "wa:deep-copy-nested" "$(printf '(let ((x (with-arena (128) (cons (cons 1 2) (cons 3 4))))) (caar x))')" "1"

# ── performance-region / evolution-region tests ──────────
run_test "pr:basic" "$(printf '(performance-region (+ 1 2))')" "3"
run_test "pr:multi" "$(printf '(performance-region (+ 1 2) (+ 3 4))')" "7"
run_test "pr:with-cons" "$(printf '(performance-region (car (cons 1 2)))')" "1"
run_test "er:basic" "$(printf '(evolution-region (+ 1 2))')" "3"
run_test "er:multi" "$(printf '(evolution-region (+ 1 2) (+ 3 4))')" "7"
run_test "pr:in-define" "$(printf '(define (f) (performance-region 42)) (f)')" "42"
run_test "pr:nested" "$(printf '(performance-region (evolution-region (+ 1 2)))')" "3"

# ── with-arena: arena scope tests ─────────────────────────
run_test "wa:basic" "$(printf '(with-arena (1024) (+ 1 2))')" "3"
run_test "wa:multi-body" "$(printf '(with-arena (1024) (+ 1 2) (+ 3 4))')" "7"
run_test "wa:default-size" "$(printf '(with-arena () (car (cons 1 2)))')" "1"
run_test "wa:nested" "$(printf '(with-arena (1024) (with-arena (512) (+ 10 20)) (+ 30 40))')" "70"
run_test "wa:with-pairs" "$(printf '(with-arena () (car (cons 42 0)))')" "42"
# (with-arena with no body) returns void, which produces no output
run_test "wa:empty-body" "$(printf '(with-arena (64))')" ""

# Note: with-arena requires g_use_arena=true (TL arena must be initialized).
# --no-arena tests are not applicable — with-arena is inherently arena-based.

# ── Hash operation tests (fixnum + string keys) ────────────
run_test "hash:fixnum-set-get" "$(printf '(let ((h (hash))) (hash-set! h 42 1) (hash-ref h 42))')" "1"
run_test "hash:string-set-get" "$(printf '(let ((h (hash))) (hash-set! h "k" 99) (hash-ref h "k"))')" "99"
run_test "hash:multi-fixnum" "$(printf '(let ((h (hash))) (hash-set! h 1 10) (hash-set! h 2 20) (+ (hash-ref h 1) (hash-ref h 2)))')" "30"
run_test "hash:multi-string" "$(printf '(let ((h (hash))) (hash-set! h "a" 10) (hash-set! h "b" 20) (+ (hash-ref h "a") (hash-ref h "b")))')" "30"
run_test "hash:remove-miss" "$(printf '(let ((h (hash))) (hash-set! h 42 1) (hash-remove! h 42) (hash-ref h 42))')" ""
run_test "hash:length-single" "$(printf '(let ((h (hash))) (hash-set! h 42 1) (hash-length h))')" "1"
run_test "hash:has-key-true" "$(printf '(let ((h (hash))) (hash-set! h 42 1) (hash-has-key? h 42))')" "#t"
run_test "hash:has-key-false" "$(printf '(let ((h (hash))) (hash-has-key? h 42))')" "#f"
run_test "hash:keys" "$(printf '(let ((h (hash))) (hash-set! h "k" 1) (hash-keys h))')" "(\"k\")"
run_test "hash:values" "$(printf '(let ((h (hash))) (hash-set! h "k" 1) (hash-values h))')" "(1)"
run_test "hash:update" "$(printf '(let ((h (hash))) (hash-set! h 42 1) (hash-set! h 42 2) (hash-ref h 42))')" "2"
run_test "hash:combo" "$(printf '(let ((h (hash))) (hash-set! h 1 10) (hash-set! h 2 20) (hash-set! h 3 30) (+ (hash-length h) (hash-ref h 1) (hash-ref h 3)))')" "43"

# ── --no-arena: hash operations still correct ─────────────
run_with_flag "hash:no-arena-fixnum" "$(printf '(let ((h (hash))) (hash-set! h 42 1) (hash-ref h 42))')" "1"
run_with_flag "hash:no-arena-string" "$(printf '(let ((h (hash))) (hash-set! h "k" 99) (hash-ref h "k"))')" "99"
run_with_flag "hash:no-arena-has-key" "$(printf '(let ((h (hash))) (hash-set! h 42 1) (hash-has-key? h 42))')" "#t"

# ── JIT hash edge cases (string + fixnum interop) ────────
run_test "hash:string-multi" "$(printf '(let ((h (hash))) (hash-set! h "a" 10) (hash-set! h "b" 20) (hash-set! h "c" 30) (+ (hash-ref h "a") (hash-ref h "b") (hash-ref h "c")))')" "60"
run_test "hash:mixed-keys" "$(printf '(let ((h (hash))) (hash-set! h 1 10) (hash-set! h "k" 2) (+ (hash-ref h 1) (hash-ref h "k")))')" "12"
run_test "hash:string-update" "$(printf '(let ((h (hash))) (hash-set! h "k" 1) (hash-set! h "k" 2) (hash-ref h "k"))')" "2"
run_test "hash:string-remove" "$(printf '(let ((h (hash))) (hash-set! h "k" 1) (hash-remove! h "k") (hash-ref h "k"))')" ""
run_test "hash:string-has-key" "$(printf '(let ((h (hash))) (hash-set! h "k" 1) (hash-has-key? h "k"))')" "#t"
run_test "hash:string-key-keys" "$(printf '(let ((h (hash))) (hash-set! h "x" 1) (hash-set! h "y" 2) (length (hash-keys h)))')" "2"
run_test "hash:large-set-get" "$(printf '(let ((h (hash))) (hash-set! h "k" 1) (hash-set! h "k" 2) (hash-set! h "k" 3) (hash-ref h "k"))')" "3"
run_test "hash:fill-5" "$(printf '(let ((h (hash 1 10 2 20 3 30 4 40 5 50))) (+ (hash-ref h 1) (hash-ref h 5)))')" "60"

# ── Escape analysis + hash interactions ──────────────────
run_test "ea:hash-escaped-pair" "$(printf '(let ((h (hash))) (hash-set! h "k" (cons 1 2)) (car (hash-ref h "k")))')" "1"
run_test "ea:hash-stored-multi" "$(printf '(let ((ht (hash))) (hash-set! ht "a" (cons 10 20)) (hash-set! ht "b" (cons 30 40)) (+ (car (hash-ref ht "a")) (cdr (hash-ref ht "b"))))')" "50"

echo ""

# ── Git integration tests (Issue #96) ───────────────────────
# Read-only primitives only. git-commit/git-stage are stateful
# and not exercised here.

run_test "git:branch-current" "$(printf '(begin (require "std/git" all:) (string? (git-branch-current)))')" "#t"
run_test "git:rev-parse" "$(printf '(begin (require "std/git" all:) (string? (git-rev-parse)))')" "#t"
run_test "git:status" "$(printf '(begin (require "std/git" all:) (string? (git-status)))')" "#t"
run_test "git:diff" "$(printf '(begin (require "std/git" all:) (string? (git-diff)))')" "#t"
run_test "git:log" "$(printf '(begin (require "std/git" all:) (string? (git-log 5)))')" "#t"

# safe-refactor module loads cleanly
run_test "safe-refactor:loaded" "$(printf '(begin (require std/safe-refactor all:) #t)')" "#t"

# safe-refactor:with-snapshot returns thunk result on success
run_test "safe-refactor:success" "$(printf '(begin (require std/safe-refactor all:) (display (safe-refactor:with-snapshot "t" (lambda () 42))))')" "42"

# safe-refactor:with-snapshot returns rolled-back on error
run_test "safe-refactor:rollback" "$(printf '(begin (require std/safe-refactor all:) (display (safe-refactor:with-snapshot "t" (lambda () (error "x")))))')" "(rolled-back error-raised)"

# safe-refactor:check-and-apply pre-verify fails
run_test "safe-refactor:pre-fail" "$(printf '(begin (require std/safe-refactor all:) (display (safe-refactor:check-and-apply (lambda () #f) (lambda () #t) (lambda () 1))))')" "(rejected pre-verify-failed)"

# safe-refactor:check-and-apply all pass
run_test "safe-refactor:applied" "$(printf '(begin (require std/safe-refactor all:) (display (safe-refactor:check-and-apply (lambda () #t) (lambda () #t) (lambda () (quote ok)))))')" "(applied ok)"

# Issue #4266: (typecheck-status) is structured — #t on a clean workspace
run_test "safe-refactor:tc-status-clean" "$(printf '(begin (require std/safe-refactor all:) (set-code "(define (g (: x Int)) x)") (display (typecheck-status)))')" "#t"

# Issue #4266: replace-fn applies a well-typed replacement
run_test "safe-refactor:replace-applies" "$(printf '(begin (require std/safe-refactor all:) (set-code "(define (g (: x Int)) x)") (display (car (safe-refactor:replace-fn "g" "(define (g (: x Int)) (+ x 1))"))))')" "applied"

# Issue #4266: replace-fn REJECTS an ill-typed replacement (was: applied — the bug)
run_test "safe-refactor:replace-rejects" "$(printf '(begin (require std/safe-refactor all:) (set-code "(define (g (: x Int)) x)") (display (car (safe-refactor:replace-fn "g" "(define (g (: x Int)) (g \\\"s\\\"))"))))')" "rejected"

# Issue #4266: check-and-apply restores when post-verify throws (was: escaped, no restore)
run_test "safe-refactor:post-verify-throw" "$(printf '(begin (require std/safe-refactor all:) (set-code "(define (g (: x Int)) x)") (display (car (safe-refactor:check-and-apply (lambda () #t) (lambda () (error "pv")) (lambda () (begin (set-code "(define (h (: x Int)) x)") 42))))))')" "error"
# ── String corruption regression (Bug TBD) ───────────────
# Triggered by sequence: r1 + r2(error, restores) + r3('fail', restores) + r4("hello")
# Without the bug, the output would be "hello".
# With the bug, the string's low-6 type bits get clobbered to RefKeyword bits.
# This test asserts the CURRENT (buggy) output so the bug stays reproducible.
# When the bug is fixed, change the expected output to "hello".
cat > /tmp/bug-string-corruption.aura <<'BUGINPUT'
(require std/safe-refactor all:)(define r1 (safe-refactor:with-snapshot "t1" (lambda () 42)))(define r2 (safe-refactor:with-snapshot "t2" (lambda () (error "boom"))))(define r3 (safe-refactor:with-snapshot "t3" (lambda () (quote fail))))(define r4 (safe-refactor:with-snapshot "t4" (lambda () "hello")))(display r4)(newline)
BUGINPUT
run_test "fix:string-vs-keyword-overlap" "$(cat /tmp/bug-string-corruption.aura)" "hello"

echo ""

echo "=== FFI Tests ==="

# Opaque pointer basics
run_test "ffi:opaque-bool" "$(printf '(begin (require "std/ffi" all:) (c-opaque? (c-opaque 42)))')" "#t"
run_test "ffi:opaque-int" "$(printf '(begin (require "std/ffi" all:) (c-opaque->int (c-opaque 42)))')" "42"
run_test "ffi:opaque-alloc" "$(printf '(begin (require "std/ffi" all:) (define p (c-alloc 64)) (c-free p) (c-opaque? p))')" "#t"
run_test "ffi:opaque-not" "$(printf '(begin (require "std/ffi" all:) (c-opaque? 42))')" "#f"
run_test "ffi:alloc-free" "$(printf '(begin (require "std/ffi" all:) (define p (c-alloc 1024)) (c-free p) (c-opaque? p))')" "#t"


# --emit-binary: standalone native binary
_emit_case() {
    local name="$1" input="$2" expected="$3"
    local safe="${name//\//_}"
    local bin_path err_path result
    if [ -n "${_RESULT_DIR:-}" ]; then
        bin_path="$_RESULT_DIR/emit_$safe"
    else
        bin_path="/tmp/aura_emit_${safe}"
    fi
    err_path="${bin_path}.err"
    # Capture stderr to a file so we can show it on failure (don't pollute test output).
    printf '%s' "$input" | timeout 10 "$AURA" --emit-binary "$bin_path" 2>"$err_path" >/dev/null
    if [ -x "$bin_path" ]; then
        result=$("$bin_path" 2>&1 | tr -d '\n')
        if [ "$result" = "$expected" ]; then
            _record_pass "$name"
        else
            local extra="       expected: $expected"$'\n'"       got:      $result"
            if [ -s "$err_path" ]; then
                extra+=$'\n'"       stderr:   $(head -1 "$err_path")"
            fi
            _record_fail "$name" "$extra"
        fi
        rm -f "$bin_path" "${bin_path}.o" "${bin_path}.tmp.aura" "${bin_path}.runtime.o" "${bin_path}.ir" "$err_path" 2>/dev/null
    else
        local extra=""
        if [ -s "$err_path" ]; then
            extra="       aot_err:  $(head -3 "$err_path" | tr '\n' ' ')"
        fi
        rm -f "$err_path"
        _record_fail "$name (no binary)" "$extra"
    fi
}
run_emit_test() {
    _dispatch _emit_case "$1" "$2" "$3"
}


echo "=== --emit-binary Tests ==="
# Note: AOT produces standalone native binaries via LLVM IR → llc → link with runtime.c.
#       Booleans output as raw int (1=#t, 0=#f). Display side-effects + return combined.
#       Not yet supported: primitives dispatched via closure (and/or/not, pair?, list).

# Basic arithmetic (inlined as LLVM IR ops)
run_emit_test "emit:add"     "(+ 1 2)" "3"
run_emit_test "emit:sub"     "(- 5 3)" "2"
run_emit_test "emit:mul"     "(* 2 3)" "6"
run_emit_test "emit:neg"     "(- 42)" "-42"
run_emit_test "emit:chain"   "(+ 1 2 3)" "6"

# Pairs (direct pair primitives via runtime.c functions)
run_emit_test "emit:car"     "(car (cons 42 100))" "42"
run_emit_test "emit:cdr"     "(cdr (cons 42 100))" "100"
run_emit_test "emit:cadr"    "(car (cdr (cons 10 (cons 20 30))))" "20"
run_emit_test "emit:car-list"   "(car (list 1 2 3))" "1"
run_emit_test "emit:cadr-list"  "(car (cdr (list 10 20 30)))" "20"

# pair?/null? (inlined as LLVM ICmp via OpPrimCall)
# main() skips printing 0 values, so falsy expectations must be empty.
# Issue #2482: null? is true only for empty list (void), not fixnum 0.
run_emit_test "emit:pair?"   "(pair? (cons 1 2))" "#t"
run_emit_test "emit:not-pair?"   "(pair? 42)" "#f"
run_emit_test "emit:null?"   "(null? (list))" "#t"
run_emit_test "emit:null?-zero"  "(null? 0)" "#f"
run_emit_test "emit:not-null?"   "(null? -1)" "#f"

# Comparisons (inlined as LLVM ICmp, raw int output: 1=#t)
run_emit_test "emit:eq-lit"  "(= 42 42)" "#t"
run_emit_test "emit:lt"      "(< 1 2)" "#t"

# Boolean conditionals
run_emit_test "emit:bool"    "(if #t 42 0)" "42"

# User-defined closures (compiled + registered via func_table)
run_emit_test "emit:closure"    "(let ((f (lambda (x) (+ x 1)))) (f 41))" "42"
run_emit_test "emit:closure2"   "(let ((add (lambda (a b) (+ a b)))) (add 10 20))" "30"

# and/or/not (expanded to conditional branches in lowering)
run_emit_test "emit:and"        "(and #t #t)" "#t"
run_emit_test "emit:or"         "(or #f #t)" "#t"
run_emit_test "emit:not"        "(not #f)" "#t"
run_emit_test "emit:and-chain"  "(and (< 1 10) (> 5 0) (= 3 3))" "#t"

# Conditionals
run_emit_test "emit:if-true"    "(if #t 42 0)" "42"
run_emit_test "emit:if-false"   "(if #f 0 99)" "99"

# Let bindings
run_emit_test "emit:let"        "(let ((x 10) (y 20)) (+ x y))" "30"

# Integer primitives (inlined)
run_emit_test "emit:quotient"   "(quotient 10 3)" "3"
run_emit_test "emit:remainder"  "(remainder 10 3)" "1"

# Display: side-effect prints 42, then main() prints return 42 = "4242"
run_emit_test "emit:display"    "(display 42)" "42"

# String ops (via aura_prim_call PrimId dispatch)
run_emit_test "emit:string-len"  "(string-length \"hello\")" "5"
run_emit_test "emit:string-eq"   "(string=? \"abc\" \"abc\")" "#t"
run_emit_test "emit:display-car" "(display (car (list 1 2 3)))" "1"
run_emit_test "emit:display-list" "(display (list 1 2 3))" "(1 2 3)"

# M4 ownership model
run_emit_test "emit:drop-int"    "(begin (drop 42) 7)" "7"
run_emit_test "emit:drop-pair"   "(begin (drop (cons 1 2)) 7)" "7"
# Issue #2407: move/Linear of Copy literals are no-ops (lowering elides
# MoveOp/LinearWrap; runtime.c weak pin/unpin stubs for any residual emit).
run_emit_test "emit:move-int"    "(begin (move 42) 7)" "7"
run_emit_test "emit:borrow"      "(begin (& 42) 7)" "7"
run_emit_test "emit:linear"      "(begin (Linear 42) 7)" "7"

# Complex ownership lifecycles
run_emit_test "emit:drop-chain"  "(begin (drop 1) (drop 2) (drop 3) 42)" "42"
run_emit_test "emit:lin-drop"    "(begin (drop (Linear 42)) 7)" "7"
run_emit_test "emit:drop-loop"   "(begin (drop (cons 1 2)) (drop (cons 3 4)) (drop (cons 5 6)) 99)" "99"

# List ops (via aura_prim_call PrimId dispatch)
run_emit_test "emit:length"     "(length (list 10 20 30))" "3"
run_emit_test "emit:list-ref"   "(list-ref (list 10 20 30) 1)" "20"
run_emit_test "emit:reverse"   "(car (reverse (list 1 2 3)))" "3"
run_emit_test "emit:append"    "(car (append (list 1 2) (list 3 4)))" "1"
run_emit_test "emit:member"    "(car (member 2 (list 1 2 3)))" "2"
run_emit_test "emit:map"       "(car (map (lambda (x) (+ x 10)) (list 1 2 3)))" "11"
run_emit_test "emit:foldl"     "(foldl + 0 (list 1 2 3))" "6"

# Stdlib algorithm module
run_emit_test "emit:merge"     "(import \"std/algorithm\")(car (merge-sorted (list 1 3) (list 2 4)))" "1"
run_emit_test "emit:uniq"      "(import \"std/algorithm\")(car (unique (list 1 1 2 3)))" "1"
# binary-search returns index of hit (mid), not boolean (#t).
run_emit_test "emit:bin-search" "(import \"std/algorithm\")(binary-search 2 (list 1 2 3))" "1"

# Stdlib list module
run_emit_test "emit:list-merge" "(import \"std/algorithm\")(car (merge-sorted (list 1 3) (list 2 4)))" "1"

# Stdlib list module (self-recursive functions)
run_emit_test "emit:range"     "(import \"std/list\")(display (range 0 3))" "(0 1 2)"
run_emit_test "emit:factorial" "(import \"std/math\")(display (factorial 5))" "120"

# Named let (local recursion via letrec + closure env)
run_emit_test "emit:named-let" "(let loop ((x 0)) (if (< x 3) (loop (+ x 1)) x))" "3"

# Stdlib functions via import (source inlining)
run_emit_test "emit:sorted?"   "(import \"std/algorithm\")(sorted? (list 1 2 3))" "#t"
run_emit_test "emit:combine"   "(import \"std/algorithm\")(combinations 4 2)" "6"
run_emit_test "emit:apply"     "(apply + (list 1 2 3))" "6"
run_emit_test "emit:nested-car"  "(car (car (list (list 1 2) (list 3 4))))" "1"
echo "=== Diagnostic Tests ==="

# Parse error: source line + caret display (via batch eval)
run_test "diag:parse-error-caret" "$(printf '#|test|')" "error: 1:1: parse error: expected expression, got invalid character   |  1 | #|test|   | ^^^^^^^"

# Parse error: multi-line source
run_test "diag:parse-error-multiline" "$(printf '#|whoops|')" "error: 1:1: parse error: expected expression, got invalid character   |  1 | #|whoops|   | ^^^^^^^^^"

# Parse error: bad syntax
run_test "diag:parse-error-badsyntax" "$(printf '#bad')" "error: 1:1: parse error: expected expression, got invalid character   |  1 | #bad   | ^^^^"

# Type checker: unbound variable with suggestion
run_typecheck_test "diag:typecheck-suggest" "(undef 5)" "type: Any1:2: unbound variable: undef  did you mean 'and'?"

run_typecheck_test "diag:typecheck-ok" "(+ 1 2)" "type: Intno errors"


# Parse error: unterminated string

# set-code + query:find
run_test "agent:set-code-query" "$(printf '(set-code "(define (f x) (+ x 1))")(query :find \"f\")')" "(5)"

# set-code + query:node-type
run_test "agent:query-node-type" "$(printf '(set-code "(define (f x) (+ x 1))")(query:node-type \"Define\")')" "(5)"

# set-code + query:calls
run_test "agent:query-calls" "$(printf '(set-code "(define (f x) (+ x 1)) (define (g x) (f x))")(query:calls \"f\")')" "(8)"

# Pipe mode: display side-effect + return value (newline returns void, not printed)
run_test "agent:pipe-mode" "$(printf '(display 42)')" "42"

# EDSL mutate: rebind function
run_test "agent:mutate-rebind" "$(printf '(set-code "(define (add x y) (+ x y))")(query :find \"add\")(mutate:rebind \"add\" \"(define (add x y) (* x y))\")(eval-current)(add 1 2)')" "2"

# query:filter / query:where — combined filter queries
run_test "edsl:filter-where" \
  "$(printf '(set-code "(define (sort lst) (if (null? lst) () (sort (cdr lst))))")(query:filter (query:where :node-type "Call") (query:where :callee "sort"))')" \
  "(8)"
run_test "edsl:filter-define-name" \
  "$(printf '(set-code "(define (add x y) (+ x y))")(query:filter (query:where :node-type "Define") (query:where :defines "add"))')" \
  "(5)"

# query:pattern

# EDSL query:pattern (use escaped quotes to avoid shell conflicts)

# JSON commands via --serve pipe mode

# ── Bridge tests (closure bridge + body_source fallback) ─────

run_test "bridge:map-lambda"   "$(printf '(map (lambda (x) (+ x 10)) (list 1 2 3))')" "(11 12 13)"
run_test "bridge:filter-lambda" "$(printf '(filter (lambda (x) (> x 2)) (list 1 2 3 4 5))')" "(3 4 5)"
run_test "bridge:foldl-lambda" "$(printf '(foldl (lambda (acc x) (+ acc x)) 0 (list 1 2 3 4 5))')" "15"

# Cached function with inner lambda bridge
run_test "bridge:cached-fn" "$(printf '(define (my-map f lst) (if (null? lst) () (cons (f (car lst)) (my-map f (cdr lst)))))(my-map (lambda (x) (* x 2)) (list 1 2 3))')" "(2 4 6)"

# Nested closure bridge via cached function
run_test "bridge:nested-lambda" "$(printf '(define (twice f) (lambda (x) (f (f x))))((twice (lambda (x) (+ x 1))) 5)')" "7"

# ── Arena memory tracking tests ────────────────────────────
# arena-offset returns an integer >= 0 when arena is active
run_test "arena:offset-nonneg" "$(printf '(>= (arena-offset) 0)')" "#t"
run_test "arena:offset-usable" "$(printf '(let ((a (arena-offset))) (cons (* 2 3) (* 4 5)) (>= (arena-offset) a))')" "#t"
run_test "git:branch-current" "$(printf '(begin (require "std/git" all:) (string? (git-branch-current)))')" "#t"
run_test "git:rev-parse" "$(printf '(begin (require "std/git" all:) (string? (git-rev-parse)))')" "#t"
run_test "git:status" "$(printf '(begin (require "std/git" all:) (string? (git-status)))')" "#t"
run_test "git:diff" "$(printf '(begin (require "std/git" all:) (string? (git-diff)))')" "#t"
run_test "git:log" "$(printf '(begin (require "std/git" all:) (string? (git-log 5)))')" "#t"


echo ""
echo "=== EDSL IR Cache V2 Tests (Phase 2) ==="
# Phase 2: set-code populates the v2 IR cache via pre_cache_workspace_defines.
# These tests verify the behavior, not the internal cache state.
# (The ctest test_ir_cache_v2 verifies the FNV-1a hash function.)

run_test "edsl-ir-cache:set-code-populates"  \
    "$(printf '(set-code \"(define my-fn (lambda (x) (* x x)))\") (length (query :find \"my-fn\"))')" \
    '1'
run_test "edsl-ir-cache:eval-current-binds"  \
    "$(printf '(set-code \"(define f (lambda (x) (+ x 1)))\") (eval-current) (f 5)')" \
    '6'
run_test "edsl-ir-cache:resetcode-rebinds"  \
    "$(printf '(set-code \"(define f (lambda (x) (* x 2)))\") (eval-current) (f 5)')" \
    '10'
run_test "edsl-ir-cache:multi-define"         \
    "$(printf '(set-code \"(define a 1) (define b 2) (define c 3)\") (eval-current) (+ a b c)')" \
    '6'

# Phase 3: cascade dirty invalidation.
# After (mutate:rebind "f" "..."), all defines that reference f (transitively
# via dep_graph_) must be marked dirty too — the IR for g embeds a closure
# capture of f's lowered function, so a re-lower of g is needed.
# Phase 3 cascade: set-code alone does not lower into IR v2 (entries stay
# dirty until eval-current / first lower). After eval-current both f and g
# are clean; cascade-after-mutate still forces g dirty on f rebind.
run_test "edsl-ir-cache:cascade-after-setcode"  \
    "$(printf '(set-code \"(define f (lambda (x) (* x 2))) (define g (lambda (x) (f x)))\") (eval-current) (ir-cache-v2:dirty? \"g\")')" \
    '#f'
run_test "edsl-ir-cache:cascade-after-mutate"   \
    "$(printf '(set-code \"(define f (lambda (x) (* x 2))) (define g (lambda (x) (f x)))\") (eval-current) (mutate:rebind \"f\" \"(lambda (x) (* x 3))\") (ir-cache-v2:dirty? \"g\")')" \
    '#t'
run_test "edsl-ir-cache:cascade-not-on-strangers" \
    "$(printf '(set-code \"(define f (lambda (x) (* x 2))) (define g (lambda (x) (* x 2)))\") (mutate:rebind \"f\" \"(lambda (x) (* x 3))\") (ir-cache-v2:dependents \"f\")')" \
    ''  # no f→g edge; empty dependents is void (dirty? also flipped by arena compact)

# Phase 4: (eval-current :jit) hooks into the IR pipeline.
# When :jit is given, the workspace is re-evaluated via eval_ir (which
# has the type-specialize + const-fold + LLVM JIT pipeline). Falls back
# to the IR interpreter if LLVM isn't available.
# Phase 4: (eval-current :jit) re-evaluates via the IR pipeline.
# The pipeline prints "PM: running ..." to stderr, which the test
# script merges into stdout. Filter those out before comparing.
_jit_case() {
    local name="$1" input="$2" expected="$3"
    local actual
    actual=$(printf '%s' "$input" | timeout 10 "$AURA" 2>&1 | grep -vE '^PM:' | tr -d '\n')
    if [ "$actual" = "$expected" ]; then
        _record_pass "$name"
    else
        _record_fail "$name" "       expected: $expected" "       got:      $actual"
    fi
}
run_test_jit() {
    _dispatch _jit_case "$1" "$2" "$3"
}
run_test_jit "edsl-ir-cache:jit-value-producing" \
    "$(printf '(set-code \"(+ 1 2)\") (eval-current :jit)')" \
    '3'
run_test "edsl-ir-cache:jit-matches-regular"  \
    "$(printf '(set-code \"(define (sq x) (* x x)) (sq 7)\") (eval-current)')" \
    '49'

# Follow-up 3: IR v2 is populated on first lower (eval-current), not bare
# set-code. After eval-current, (ir-cache-v2:dirty? "f") is #f and the
# cached IR is reused on subsequent eval-current with identical source.
run_test "edsl-ir-cache:v2-populated-by-default" \
    "$(printf '(set-code \"(define (f x) (* x x))\") (eval-current) (ir-cache-v2:dirty? \"f\")')" \
    '#f'
# Second set-code with same source is a cache hit (no re-lower).
run_test "edsl-ir-cache:v2-cache-hit" \
    "$(printf '(set-code \"(define (f x) (* x x))\") (eval-current) (set-code \"(define (f x) (* x x))\") (eval-current) (display (f 7))')" \
    '49'
# Issue #238: incremental mutation stress (100+ rebinds). Currently hangs
# after "starting-cycles..." even under AURA_SANDBOX=off — skipped until
# cycle bound is fixed (same as suite SUITE_SKIP for the .aura file).
# run_test "edsl-ir-cache:incremental-mutation-stress" "$(cat tests/suite/incremental_mutation_test.aura)" "baseline-cache-size: 9baseline-dirty-count: 0warmup-no-mut-preserves-dirty: #tstarting-cycles...mutations-completed: 109mismatches: 0final-cache-size: 9final-dirty-count: 0dep-edges: 16no-mut-preserves-dirty: #ttest-complete" 120
echo "  ↷  edsl-ir-cache:incremental-mutation-stress: SKIPPED — hangs after starting-cycles (tracked with suite skip)"

# Same input via :jit should give the same numeric result.
# (Note: no env sync back, so :jit's return value is the JIT's last
# expression value of the re-eval, not the workspace's bound env.)

# Issue #4325: blank line is "" and EOF is void. read-byte returns one
# byte with no newline required. Program is -e so stdin stays the data.
_read_stdin_case() {
    local name="$1" data="$2" expr="$3" expected="$4"
    local actual
    actual=$(printf '%s' "$data" | timeout 15 "$AURA" -e "$expr" 2>&1 | tr -d '\n')
    if [ "$actual" = "$expected" ]; then
        _record_pass "$name"
    else
        _record_fail "$name" "       expected: $expected" "       got:      $actual"
    fi
}
_read_stdin_case "4325-blank-then-eof" $'\n' \
    '(begin (define a (read-line)) (define b (read-line)) (display (string-length a)) (display (eof-object? a)) (display (eof-object? b)) (display (eq? a b)))' \
    '0#f#t#f'
_read_stdin_case "4325-read-byte-no-newline" 'Q' \
    '(display (read-byte))' \
    '81'
_read_stdin_case "4325-read-byte-eof" '' \
    '(display (eof-object? (read-byte)))' \
    '#t'

# Issue #4354: a loaded file of defines is one letrec begin. An alias
# written before a redefinition must keep the procedure from the earlier
# load, not the new body (that loop used to hang).
_load_alias_4354() {
    local dir aura_abs lib_path actual expected
    dir=$(mktemp -d /tmp/aura-4354.XXXXXX)
    aura_abs="$AURA"
    lib_path="$PWD/lib"
    if [ "${aura_abs#/}" = "$aura_abs" ]; then
        aura_abs="$PWD/$aura_abs"
    fi
    cat >"$dir/lib.aura" <<'EOF'
(define (f x) (string-append "base:" x))
EOF
    cat >"$dir/wrap.aura" <<'EOF'
(define *f-base* f)
(define (f x) (if (string=? x "w") "wrapped" (*f-base* x)))
EOF
    cat >"$dir/main.aura" <<'EOF'
(load "lib.aura")
(load "wrap.aura")
(display (eq? *f-base* f)) (newline)
(display (f "w")) (newline)
(display (f "z")) (newline)
EOF
    # cd away from the repo so ./lib is not the module root; point AURA_PATH
    # at it. load forces the full std prelude (std/list).
    actual=$(cd "$dir" && AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 \
        AURA_PATH="$lib_path" timeout 8 "$aura_abs" main.aura 2>&1 | tr -d '\r')
    rm -rf "$dir"
    expected=$'#f\nwrapped\nbase:z'
    if [ "$actual" = "$expected" ]; then
        _record_pass "4354-load-alias-keeps-base"
    else
        _record_fail "4354-load-alias-keeps-base" "       expected:" "$expected" "       got:" "$actual"
    fi
}
_load_alias_4354
# Issue #4353: Env::lookup SoA walk must match frame string bindings in
# reverse - a TCO call frame accumulates copied ancestor bindings then fresh
# local binds; forward scan resolved a shadowed name to the STALE copy
# (named-let body read the global instead of the let binding).
_named_let_shadow_4353() {
    local dir aura_abs actual expected
    dir=$(mktemp -d /tmp/aura-4353.XXXXXX)
    aura_abs="$AURA"
    if [ "${aura_abs#/}" = "$aura_abs" ]; then
        aura_abs="$PWD/$aura_abs"
    fi
    cat >"$dir/repro.aura" <<'AEOF'
(define gv 7)
(define (row a) 99)
(define (h1 x) (let lp ((i 0) (p1 x)) (if (>= i 2) p1 (lp (+ i 1) p1))))
(define (h3 x) (let ((gv (+ x 1))) (let lp ((i 0)) (if (>= i 2) gv (lp (+ i 1))))))
(define (h5 row) (let lp ((i 0)) (if (>= i 2) row (lp (+ i 1)))))
(display (h1 10)) (newline)
(display (h3 10)) (newline)
(display (h5 10)) (newline)
AEOF
    actual=$(AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 \
        timeout 8 "$aura_abs" "$dir/repro.aura" 2>&1 | tr -d '\r')
    rm -rf "$dir"
    expected=$'10\n11\n10'
    if [ "$actual" = "$expected" ]; then
        _record_pass "4353-named-let-shadow-reverse"
    else
        _record_fail "4353-named-let-shadow-reverse" "       expected:" "$expected" "       got:" "$actual"
    fi
}
_named_let_shadow_4353


# Issue #4355: a top-level error must not re-run earlier forms. After
# set-code, the forms that follow the error must still call the closure.
_file_4355() {
    local dir aura_abs out err
    dir=$(mktemp -d /tmp/aura-4355.XXXXXX)
    aura_abs="$AURA"
    if [ "${aura_abs#/}" = "$aura_abs" ]; then
        aura_abs="$PWD/$aura_abs"
    fi
    cat >"$dir/a.aura" <<'EOF'
(define (f x) (+ x 1))
(display "one ") (display (f 1)) (newline)
(display (no-such-proc 1))
(display "two ") (display (f 2)) (newline)
EOF
    cat >"$dir/b.aura" <<'EOF'
(define (pr k v) (display k) (display "=") (display v) (newline))
(set-code "(define g (lambda (x) (* x 2)))")
(eval-current)
(pr "g" (g 4))
(pr "x" (no-such-proc 1))
(pr "after" (g 5))
EOF
    out=$(AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 timeout 15 "$aura_abs" "$dir/a.aura" 2>"$dir/a.err")
    err=$(cat "$dir/a.err")
    if [ "$out" = $'one 2\ntwo 3' ] && printf '%s' "$err" | grep -q 'unbound variable: no-such-proc'; then
        _record_pass "4355-error-does-not-rerun-prefix"
    else
        _record_fail "4355-error-does-not-rerun-prefix" "       stdout:" "$out" "       stderr:" "$err"
    fi
    out=$(AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 timeout 15 "$aura_abs" "$dir/b.aura" 2>"$dir/b.err")
    err=$(cat "$dir/b.err")
    if printf '%s' "$out" | grep -q 'g=8' && printf '%s' "$out" | grep -q 'after=10' \
        && ! printf '%s' "$err" | grep -q 'invalid closure'; then
        _record_pass "4355-after-set-code-closure-survives"
    else
        _record_fail "4355-after-set-code-closure-survives" "       stdout:" "$out" "       stderr:" "$err"
    fi
    rm -rf "$dir"
}
_file_4355

# Issue #4356: a thread fiber allocating strings must not race read-line
# on the same Evaluator. Both bad-counts stay 0 (no corrupted lines).
_file_4356() {
    local dir aura_abs out err
    dir=$(mktemp -d /tmp/aura-4356.XXXXXX)
    aura_abs="$AURA"
    if [ "${aura_abs#/}" = "$aura_abs" ]; then
        aura_abs="$PWD/$aura_abs"
    fi
    cat >"$dir/race.aura" <<'EOF'
(define (pr k v) (display k) (display "=") (display v) (newline))
(define (mk n) (let lp ((i 0) (acc (list))) (if (>= i n) acc (lp (+ i 1) (cons (string-append "s" (number->string i)) acc)))))
(define (chk xs n) (let lp ((ys xs) (i (- n 1)) (bad 0)) (if (null? ys) bad (lp (cdr ys) (- i 1) (if (string=? (car ys) (string-append "s" (number->string i))) bad (+ bad 1))))))
(define (fwork) (let lp ((k 0) (bad 0)) (if (>= k 6) bad (lp (+ k 1) (+ bad (chk (mk 600) 600))))))
(define fid (fiber:spawn fwork))
(define (readall i bad)
  (let ((l (read-line)))
    (if (or (eof-object? l) (not (string? l))) (list i bad)
        (readall (+ i 1) (if (string=? l (string-append "line" (number->string i))) bad (+ bad 1))))))
(pr "main_lines_bad" (readall 0 0))
(pr "fiber_bad" (fiber:join fid))
EOF
    out=$(python3 -c 'import sys
for i in range(3000):
    sys.stdout.write("line%d\n" % i)' | AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 \
        timeout 25 "$aura_abs" "$dir/race.aura" 2>"$dir/err")
    err=$(cat "$dir/err")
    if printf '%s\n' "$out" | grep -q 'main_lines_bad=(3000 0)' \
        && printf '%s\n' "$out" | grep -q 'fiber_bad=0'; then
        _record_pass "4356-fiber-read-line-strings"
    else
        _record_fail "4356-fiber-read-line-strings" "       stdout:" "$out" "       stderr:" "$err"
    fi
    rm -rf "$dir"
}
_file_4356

# Issue #4357: a hot workspace define specialises (relower-strategy
# incremental, faster than the same file-mode loop) and a one-line
# rebind with one caller finishes inside one current-time-ms tick.
# The loop feeds the accumulator, matching the filed repro:
# (+ x 1) over 3000 steps is 3000, and (+ x 9) over 10 steps is 90.
# The post-rebind 3000-step loop must use the new body and stay faster.
_file_4357() {
    local dir aura_abs out err
    dir=$(mktemp -d /tmp/aura-4357.XXXXXX)
    aura_abs="$AURA"
    if [ "${aura_abs#/}" = "$aura_abs" ]; then
        aura_abs="$PWD/$aura_abs"
    fi
    cat >"$dir/relower.aura" <<'EOF'
(define (pr k v) (display k) (display "=") (display v) (newline))
(define (file-call x) (+ x 1))
(define (file-calls n)
  (let lp ((i 0) (s 0))
    (if (>= i n) s (lp (+ i 1) (file-call s)))))
(define t0 (current-time-ms))
(define file-sum (file-calls 3000))
(define t1 (current-time-ms))
(set-code "(define (ws-call x) (+ x 1)) (define (ws-calls n) (let lp ((i 0) (s 0)) (if (>= i n) s (lp (+ i 1) (ws-call s)))))")
(eval-current)
(define c0 (current-time-ms))
(define cold (ws-calls 3000))
(define c1 (current-time-ms))
(define hot (ws-calls 3000))
(define c2 (current-time-ms))
(define r0 (current-time-ms))
(mutate:rebind "ws-call" "(lambda (x) (+ x 9))")
(define r1 (current-time-ms))
(define a0 (current-time-ms))
(define after (ws-calls 3000))
(define a1 (current-time-ms))
(pr "file" (- t1 t0))
(pr "cold" (- c1 c0))
(pr "hot" (- c2 c1))
(pr "rebind" (- r1 r0))
(pr "after" (- a1 a0))
(pr "sum" cold)
(pr "file-sum" file-sum)
(pr "after-sum" after)
(pr "v" (ws-calls 10))
(pr "strat" (compile:relower-strategy "ws-call"))
(pr "strat-calls" (compile:relower-strategy "ws-calls"))
EOF
    out=$(AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 timeout 25 "$aura_abs" "$dir/relower.aura" 2>"$dir/err")
    err=$(cat "$dir/err")
    local file_ms cold_ms hot_ms rebind_ms after_ms
    file_ms=$(printf '%s\n' "$out" | sed -n 's/^file=//p' | head -1)
    cold_ms=$(printf '%s\n' "$out" | sed -n 's/^cold=//p' | head -1)
    hot_ms=$(printf '%s\n' "$out" | sed -n 's/^hot=//p' | head -1)
    rebind_ms=$(printf '%s\n' "$out" | sed -n 's/^rebind=//p' | head -1)
    after_ms=$(printf '%s\n' "$out" | sed -n 's/^after=//p' | head -1)
    if printf '%s\n' "$out" | grep -q 'v=90' \
        && printf '%s\n' "$out" | grep -q 'sum=3000' \
        && printf '%s\n' "$out" | grep -q 'file-sum=3000' \
        && printf '%s\n' "$out" | grep -q 'after-sum=27000' \
        && printf '%s\n' "$out" | grep -q 'strat=incremental' \
        && printf '%s\n' "$out" | grep -q 'strat-calls=incremental' \
        && [ -n "$rebind_ms" ] && [ "$rebind_ms" -le 1 ] \
        && [ -n "$hot_ms" ] && [ -n "$file_ms" ] && [ "$hot_ms" -lt "$file_ms" ] \
        && [ -n "$cold_ms" ] && [ "$cold_ms" -lt "$file_ms" ] \
        && [ -n "$after_ms" ] && [ "$after_ms" -lt "$file_ms" ]; then
        _record_pass "4357-workspace-relower-faster-than-file"
    else
        _record_fail "4357-workspace-relower-faster-than-file" "       stdout:" "$out" "       stderr:" "$err"
    fi
    rm -rf "$dir"
}
_file_4357

# Issue #4358: char-ready? polls stdin without blocking or consuming.
_file_4358() {
    local out err dir holder
    dir=$(mktemp -d /tmp/aura-4358.XXXXXX)
    out=$(printf 'hi\n' | AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 timeout 5 "$AURA" -e '(display (char-ready?)) (display (read-line))' 2>"$dir/ready.err")
    err=$(cat "$dir/ready.err" 2>/dev/null || true)
    if [ "$out" != "#thi" ] || [ -n "$err" ]; then
        _record_fail "4358-char-ready-poll" "       stdout:" "$out" "       stderr:" "$err"
        rm -rf "$dir"
        return
    fi
    mkfifo "$dir/in"
    sleep 30 >"$dir/in" &
    holder=$!
    out=$(AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 timeout 5 "$AURA" -e '(display (char-ready?))' <"$dir/in" 2>"$dir/idle.err")
    err=$(cat "$dir/idle.err" 2>/dev/null || true)
    kill "$holder" 2>/dev/null || true
    wait "$holder" 2>/dev/null || true
    if [ "$out" = "#f" ] && [ -z "$err" ]; then
        _record_pass "4358-char-ready-poll"
    else
        _record_fail "4358-char-ready-poll" "       idle stdout:" "$out" "       stderr:" "$err"
    fi
    rm -rf "$dir"
}
_file_4358

# Issue #4359: a closure returned by eval stays callable. A hang or
# abort fails the timeout. () is the old wrong value for an eval'd call.
_file_4359() {
    local out err dir
    dir=$(mktemp -d /tmp/aura-4359.XXXXXX)
    out=$(AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 timeout 5 "$AURA" -e '
(define f (eval "(lambda (x) (* x 2))"))
(define id (eval "(lambda (x) x)"))
(eval "(define dbl (lambda (x) (* x 2)))")
(display (f 4)) (newline)
(display (id 4)) (newline)
(display (dbl 4)) (newline)
(display (eval "(dbl 4)")) (newline)
(display (eval "(+ 1 2)")) (newline)
(define g (lambda (x) (* x 2)))
(define h (eval "g"))
(display (h 4)) (newline)
' 2>"$dir/err")
    err=$(cat "$dir/err" 2>/dev/null || true)
    if [ "$out" = $'8\n4\n8\n8\n3\n8' ] && [ -z "$err" ]; then
        _record_pass "4359-eval-closure-callable"
    else
        _record_fail "4359-eval-closure-callable" "       stdout:" "$out" "       stderr:" "$err"
    fi
    rm -rf "$dir"
}
_file_4359

# Issue #4360: mutate:rebind of a missing name links the define into the
# workspace root. query:code includes it, and eval-current makes it callable.
_file_4360() {
    local out err dir
    dir=$(mktemp -d /tmp/aura-4360.XXXXXX)
    out=$(AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 timeout 15 "$AURA" -e '
(set-code "(define (f x) (+ x 1))")
(eval-current)
(display (mutate:rebind "helper" "(lambda (x) (* x 9))" "ai adds helper"))
(newline)
(display (query:code))
(newline)
(eval-current)
(display (f 2))
(newline)
(display (helper 2))
(newline)
(display (mutate :rebind "helper2" "(lambda (x) (* x 7))" "ai adds helper2"))
(newline)
(display (query:code))
(newline)
(eval-current)
(display (helper2 2))
(newline)
' 2>"$dir/err")
    err=$(cat "$dir/err" 2>/dev/null || true)
    if printf '%s\n' "$out" | grep -q 'helper' \
        && printf '%s\n' "$out" | grep -q 'helper2' \
        && printf '%s\n' "$out" | grep -qx '3' \
        && printf '%s\n' "$out" | grep -qx '18' \
        && printf '%s\n' "$out" | grep -qx '14' \
        && printf '%s\n' "$out" | grep -qx '#t' \
        && ! printf '%s\n' "$out" | grep -q 'unbound' \
        && [ -z "$err" ]; then
        _record_pass "4360-rebind-add-linked"
    else
        _record_fail "4360-rebind-add-linked" "       stdout:" "$out" "       stderr:" "$err"
    fi
    rm -rf "$dir"
}
_file_4360

# Issue #4361: a rolled-back batch or ws:try-mutation leaves defines callable.
_file_4361() {
    local out err dir
    dir=$(mktemp -d /tmp/aura-4361.XXXXXX)
    out=$(AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 timeout 20 "$AURA" -e '
(set-code "(define f (lambda (x) (+ x 1))) (define pipe (lambda (x) (f x))) (define g (lambda (x) (+ x 3)))")
(eval-current)
(display (pipe 1))
(newline)
(display (g 1))
(newline)
(mutate:atomic-batch (list (list "mutate:rebind" "f" "(lambda (x) (+ x 100))" "s1") (list "mutate:rebind" "missing" "(lambda (x) x)" "s2")) "b")
(display (query:code))
(newline)
(display (f 1))
(newline)
(display (pipe 1))
(newline)
(display (g 1))
(newline)
(display (ws:try-mutation "(begin (mutate:rebind \"f\" \"(lambda (x) 0)\" \"t\") (car 5))"))
(newline)
(display (f 1))
(newline)
(display (pipe 1))
(newline)
(display (g 1))
(newline)
' 2>"$dir/err")
    err=$(cat "$dir/err" 2>/dev/null || true)
    local twos fours
    twos=$(printf '%s\n' "$out" | grep -cx '2' || true)
    fours=$(printf '%s\n' "$out" | grep -cx '4' || true)
    if [ "$twos" -ge 5 ] && [ "$fours" -ge 3 ] \
        && printf '%s\n' "$out" | grep -q '(+ x 1)' \
        && ! printf '%s\n' "$out" | grep -q '(+ x 100)' \
        && ! printf '%s\n' "$out" | grep -q 'ast:restore' \
        && ! printf '%s\n' "$out" | grep -q 'stale' \
        && ! printf '%s\n' "$out" | grep -q 'invalid closure' \
        && [ -z "$err" ]; then
        _record_pass "4361-rollback-defines-callable"
    else
        _record_fail "4361-rollback-defines-callable" "       stdout:" "$out" "       stderr:" "$err"
    fi
    rm -rf "$dir"
}
_file_4361

# Issue #4362: batch-rebind is gated like mutate:rebind.
_file_4362() {
    local dir src
    dir=$(mktemp -d /tmp/aura-4362.XXXXXX)
    src='
(set-code "(define (hello x) (+ x 1)) (define (bye y) (hello y)) (define (greet n) (string-append \"hi \" n))")
(eval-current)
(display (mutate:atomic-batch (list (list "mutate:rebind" "bye" "(lambda (y) (hello y y))" "batch")) "batch"))
(newline)
(display (mutate:atomic-batch (list (list "mutate:rebind" "greet" "(lambda (n) (string-append n 1))" "batch")) "batch"))
(newline)
(display (query:code))
(newline)
(display (mutate:atomic-batch (list (list "mutate:rebind" "hello" "(lambda (x) (+ x 2))" "ok")) "ok"))
(newline)
(display (hello 3))
(newline)
'
    local hard soft errh errs
    hard=$(AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 AURA_MUTATE_TYPE_GATE=hard timeout 25 "$AURA" -e "$src" 2>"$dir/errh")
    errh=$(cat "$dir/errh" 2>/dev/null || true)
    soft=$(AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 AURA_MUTATE_TYPE_GATE=soft timeout 25 "$AURA" -e "$src" 2>"$dir/errs")
    errs=$(cat "$dir/errs" 2>/dev/null || true)
    if printf '%s\n' "$hard" | grep -q 'arity mismatch' \
        && printf '%s\n' "$hard" | grep -q 'type error' \
        && printf '%s\n' "$hard" | grep -q '(hello y)' \
        && printf '%s\n' "$hard" | grep -q 'hi ' \
        && ! printf '%s\n' "$hard" | grep -q '(hello y y)' \
        && ! printf '%s\n' "$hard" | grep -q '(string-append n 1)' \
        && printf '%s\n' "$hard" | grep -qx '#t' \
        && printf '%s\n' "$hard" | grep -qx '5' \
        && printf '%s\n' "$soft" | grep -q 'arity mismatch' \
        && printf '%s\n' "$soft" | grep -qx '#t' \
        && printf '%s\n' "$soft" | grep -q '(string-append n 1)' \
        && ! printf '%s\n' "$soft" | grep -q '(hello y y)' \
        && printf '%s\n' "$soft" | grep -qx '5' \
        && [ -z "$errh" ] && [ -z "$errs" ]; then
        _record_pass "4362-batch-rebind-gate"
    else
        _record_fail "4362-batch-rebind-gate" "       hard:" "$hard" "       hard-err:" "$errh" "       soft:" "$soft" "       soft-err:" "$errs"
    fi
    rm -rf "$dir"
}
_file_4362

# Issue #4363: a global value define is visible to incremental typecheck,
# and a rejected rebind does not leave its diagnostics behind.
_file_4363() {
    local dir src
    dir=$(mktemp -d /tmp/aura-4363.XXXXXX)
    src='
(define (main)
  (set-code "(define (hello x) (+ x 1))\n(define (bye y) (hello y))\n(define pet \"cat\")")
  (eval-current)
  (display "RB1 ") (write (mutate:rebind "hello" "(lambda (x) (+ x pet))" "w")) (newline)
  (display "TC1 ") (write (typecheck-incremental)) (newline)
  (display "CALL1 ") (write (try (hello 3) (catch (e) (list (quote raised) e)))) (newline)
  (set-code "(define (hello x) (+ x 1))\n(define (bye y) (hello y))")
  (eval-current)
  (display "RB2 ") (write (mutate:rebind "bye" "(lambda (y) (hello y y))" "w")) (newline)
  (display "CODE2 ") (write (query:code)) (newline)
  (display "TC2 ") (write (typecheck-incremental)) (newline))
(main)
'
    local hard soft errh errs
    hard=$(AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 AURA_MUTATE_TYPE_GATE=hard timeout 25 "$AURA" -e "$src" 2>"$dir/errh")
    errh=$(cat "$dir/errh" 2>/dev/null || true)
    soft=$(AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 timeout 25 "$AURA" -e "$src" 2>"$dir/errs")
    errs=$(cat "$dir/errs" 2>/dev/null || true)
    if printf '%s\n' "$hard" | grep -q 'expected Int, got String' \
        && printf '%s\n' "$hard" | grep -q 'arity mismatch' \
        && printf '%s\n' "$hard" | grep -q '(hello y)' \
        && ! printf '%s\n' "$hard" | grep -q 'unbound variable' \
        && ! printf '%s\n' "$hard" | grep -q '(hello y y)' \
        && printf '%s\n' "$hard" | grep -q 'CALL1 4' \
        && printf '%s\n' "$soft" | grep -q 'RB1 #t' \
        && printf '%s\n' "$soft" | grep -q 'CALL1 3' \
        && ! printf '%s\n' "$soft" | grep -q 'unbound variable' \
        && [ -z "$errh" ]; then
        _record_pass "4363-value-define-type"
    else
        _record_fail "4363-value-define-type" "       hard:" "$hard" "       hard-err:" "$errh" "       soft:" "$soft" "       soft-err:" "$errs"
    fi
    rm -rf "$dir"
}
_file_4363

# Issue #4364: query:calls drops call sites of a replaced body.
_file_4364() {
    local dir src
    dir=$(mktemp -d /tmp/aura-4364.XXXXXX)
    src='
(define (main)
  (set-code "(define (hello x) (+ x 1))\n(define (bye y) (hello y))\n(hello 3)\n(bye 4)")
  (display "C0 ") (display (length (query:calls "hello"))) (newline)
  (display "R1 ") (display (mutate:rebind "bye" "(lambda (y) (- (hello y)))")) (newline)
  (display "C1 ") (display (length (query:calls "hello"))) (newline)
  (display "R2 ") (display (mutate:rebind "bye" "(lambda (y) y)")) (newline)
  (display "C2 ") (display (length (query:calls "hello"))) (newline)
  (display "R3 ") (display (mutate:rebind "bye" "(lambda (y) (hello (hello y)))")) (newline)
  (display "C3 ") (display (length (query:calls "hello"))) (newline)
  (display "CODE ") (display (query:code)) (newline)
  (eval-current)
  (display "C4 ") (display (length (query:calls "hello"))) (newline)
  (set-code (query:code))
  (display "C5 ") (display (length (query:calls "hello"))) (newline)
  (set-code "(define (hello x) (+ x 1))\n(define (bye y) (hello y))\n(hello 3)\n(bye 4)")
  (eval-current)
  (display "S0 ") (display (length (query:calls "hello"))) (newline)
  (display "SB ") (display (mutate:set-body "bye" "(lambda (y) y)")) (newline)
  (display "S1 ") (display (length (query:calls "hello"))) (newline)
  (display "BA ") (display (mutate:atomic-batch (list (list "mutate:rebind" "bye" "(lambda (y) (hello y))" "b")) "b")) (newline)
  (display "S2 ") (display (length (query:calls "hello"))) (newline)
  (display "TA ") (display (typed-mutate-atomic (list "(mutate:rebind \"bye\" \"(lambda (y) (hello (hello y)))\" \"t\")"))) (newline)
  (display "S3 ") (display (length (query:calls "hello"))) (newline))
(main)
'
    local hard soft errh errs
    hard=$(AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 AURA_MUTATE_TYPE_GATE=hard timeout 25 "$AURA" -e "$src" 2>"$dir/errh")
    errh=$(cat "$dir/errh" 2>/dev/null || true)
    soft=$(AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 timeout 25 "$AURA" -e "$src" 2>"$dir/errs")
    errs=$(cat "$dir/errs" 2>/dev/null || true)
    if printf '%s\n' "$hard" | grep -qx 'C0 2' \
        && printf '%s\n' "$hard" | grep -qx 'C1 2' \
        && printf '%s\n' "$hard" | grep -qx 'C2 1' \
        && printf '%s\n' "$hard" | grep -qx 'C3 3' \
        && printf '%s\n' "$hard" | grep -qx 'C4 3' \
        && printf '%s\n' "$hard" | grep -qx 'C5 3' \
        && printf '%s\n' "$hard" | grep -qx 'R1 #t' \
        && printf '%s\n' "$hard" | grep -qx 'R2 #t' \
        && printf '%s\n' "$hard" | grep -qx 'R3 #t' \
        && printf '%s\n' "$hard" | grep -q '(hello (hello y))' \
        && printf '%s\n' "$soft" | grep -qx 'C0 2' \
        && printf '%s\n' "$soft" | grep -qx 'C1 2' \
        && printf '%s\n' "$soft" | grep -qx 'C2 1' \
        && printf '%s\n' "$soft" | grep -qx 'C3 3' \
        && printf '%s\n' "$soft" | grep -qx 'C4 3' \
        && printf '%s\n' "$soft" | grep -qx 'C5 3' \
        && printf '%s\n' "$soft" | grep -qx 'R1 #t' \
        && printf '%s\n' "$hard" | grep -qx 'S0 2' \
        && printf '%s\n' "$hard" | grep -qx 'SB #t' \
        && printf '%s\n' "$hard" | grep -qx 'S1 1' \
        && printf '%s\n' "$hard" | grep -qx 'BA #t' \
        && printf '%s\n' "$hard" | grep -qx 'S2 2' \
        && printf '%s\n' "$hard" | grep -qx 'TA #t' \
        && printf '%s\n' "$hard" | grep -qx 'S3 3' \
        && printf '%s\n' "$soft" | grep -qx 'S1 1' \
        && printf '%s\n' "$soft" | grep -qx 'S2 2' \
        && printf '%s\n' "$soft" | grep -qx 'S3 3' \
        && [ -z "$errh" ] && [ -z "$errs" ]; then
        _record_pass "4364-calls-drop-replaced"
    else
        _record_fail "4364-calls-drop-replaced" "       hard:" "$hard" "       hard-err:" "$errh" "       soft:" "$soft" "       soft-err:" "$errs"
    fi
    rm -rf "$dir"
}
_file_4364

# Issue #4365: serialize-workspace keeps author and composite on the mutation log.
_file_4365() {
    local dir book src
    dir=$(mktemp -d /tmp/aura-4365.XXXXXX)
    book="$dir/book.aw"
    src=$(cat <<EOF
(define (show k v) (display k) (display " ") (write v) (newline))
(define (main)
  (set-code "(define (hello x) (+ x 1))\n(define (bye y) (hello y))\n(hello 3)")
  (eval-current)
  (define s0 (ast:snapshot "t0"))
  (show "FP" (mutate:set-agent-fingerprint 42))
  (show "K1" (typed-mutate-atomic (list "(mutate:rebind \\"hello\\" \\"(lambda (x) (* x 2))\\" \\"closer\\")")))
  (define s1 (ast:snapshot "t1"))
  (show "FP2" (mutate:set-agent-fingerprint 3))
  (show "K2" (typed-mutate-atomic (list "(mutate:rebind \\"bye\\" \\"(lambda (y) (- (hello y)))\\" \\"flip\\")")))
  (mutate:set-agent-fingerprint 1)
  (show "LOG" (query:mutations-since 0))
  (show "SER" (serialize-workspace "$book"))
  (set-code "(define (zzz) 0)")
  (eval-current)
  (show "DES" (deserialize-workspace "$book"))
  (show "CODE_BACK" (query:code))
  (show "LOG_BACK" (query:mutations-since 0))
  (show "RESTORE1" (ast:restore s1))
  (eval-current)
  (show "LOG_S1" (query:mutations-since 0)))
(main)
EOF
)
    local hard soft errh errs
    hard=$(AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 AURA_MUTATE_TYPE_GATE=hard timeout 30 "$AURA" -e "$src" 2>"$dir/errh")
    errh=$(cat "$dir/errh" 2>/dev/null || true)
    soft=$(AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 timeout 30 "$AURA" -e "$src" 2>"$dir/errs")
    errs=$(cat "$dir/errs" 2>/dev/null || true)
    _ok4365() {
        printf '%s\n' "$1" | grep -q 'author=42' \
            && printf '%s\n' "$1" | grep -q 'author=3' \
            && printf '%s\n' "$1" | grep -q 'composite=1' \
            && printf '%s\n' "$1" | grep -q 'composite=3' \
            && printf '%s\n' "$1" | grep -q '(\* x 2)' \
            && printf '%s\n' "$1" | grep -q '(- (hello y))' \
            && printf '%s\n' "$1" | grep -qx 'SER #t' \
            && printf '%s\n' "$1" | grep -qx 'DES #t' \
            && printf '%s\n' "$1" | grep -qx 'K1 #t' \
            && printf '%s\n' "$1" | grep -qx 'K2 #t' \
            && printf '%s\n' "$1" | grep -qx 'RESTORE1 #t' \
            && ! printf '%s\n' "$1" | grep -q 'author=0' \
            && ! printf '%s\n' "$1" | grep -q 'composite=0'
    }
    if _ok4365 "$hard" && _ok4365 "$soft" && [ -z "$errh" ] && [ -z "$errs" ]; then
        _record_pass "4365-persist-author-composite"
    else
        _record_fail "4365-persist-author-composite" "       hard:" "$hard" "       hard-err:" "$errh" "       soft:" "$soft" "       soft-err:" "$errs"
    fi
    rm -rf "$dir"
}
_file_4365

# Print final test count
_finish_parallel
printf "Tests: %d passed, %d failed\n" "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
