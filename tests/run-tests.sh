#!/bin/bash
# Aura Test Runner — runs .aura test files and checks output
# Each test is a .aura file that outputs the result on stdout.
# Exit code 0 = all pass, 1 = some failed.

AURA="${AURA:-./build/aura}"
PASS=0
FAIL=0

green() { printf "  \033[32m✓\033[0m %s\n" "$1"; }
red()   { printf "  \033[31m✗\033[0m %s\n" "$1"; }

run_test() {
    local name="$1"
    local input="$2"
    local expected="$3"
    local result

    result=$(printf '%s' "$input" | timeout 5 "$AURA" 2>&1 | tr -d '\n')
    if [ "$result" = "$expected" ]; then
        green "$name"
        PASS=$((PASS + 1))
    else
        red "$name"
        echo "       expected: $expected"
        echo "       got:      $result"
        FAIL=$((FAIL + 1))
    fi
}

# Run a typecheck test (passes expression as --typecheck argument)
run_typecheck_test() {
    local name="$1"
    local input="$2"
    local expected="$3"
    local result
    result=$(timeout 5 "$AURA" --typecheck "$input" 2>&1 | tr -d '\n')
    if [ "$result" = "$expected" ]; then
        green "$name"
        PASS=$((PASS + 1))
    else
        red "$name"
        echo "       expected: $expected"
        echo "       got:      $result"
        FAIL=$((FAIL + 1))
    fi
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
run_test "stdlib:last"   "$(printf '(import \"std/list\")(last (list 1 2 3))')" "3"
run_test "stdlib:zip"    "$(printf '(import \"std/list\")(zip (list 1 2) (list 3 4))')" "((1 3) (2 4))"

run_test "stdlib:square" "$(printf '(import \"std/math\")(square 5)')" "25"
run_test "stdlib:sqrt"   "$(printf '(import \"std/math\")(>= (sqrt 16) 4.0)')" "#t"
run_test "stdlib:fact"   "$(printf '(import \"std/math\")(factorial 5)')" "120"
run_test "stdlib:pi"     "$(printf '(import \"std/math\") (pi)')" "3.141592653589793"

run_test "stdlib:trim"   "$(printf '(require std/string all:)(string-trim \"  hi  \")')" "\"hi\""
run_test "stdlib:split"  "$(printf '(require std/string all:)(string-split \"a,b\" \",\")')" "(\"a\" \"b\")"

echo ""
echo "=== EDSL Tests ==="

run_test "edsl:set-code"     "$(printf '(set-code \"(define (f x) (+ x 1))\")')" "#t"
run_test "edsl:find"         "$(printf '(set-code \"(define (f x) (+ x 1))\") (query:find \"f\")')" "(5)"
run_test "edsl:node-type"    "$(printf '(set-code \"(define (f x) (+ x 1))\") (query:node-type \"Define\")')" "(5)"

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
echo "============"
printf "Tests: %d passed, %d failed\n" "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1

echo ""

echo "=== FFI Tests ==="

# Opaque pointer basics
run_test "ffi:opaque-bool" "$(printf '(c-opaque? (c-opaque 42))')" "#t"
run_test "ffi:opaque-int" "$(printf '(c-opaque->int (c-opaque 42))')" "42"
run_test "ffi:opaque-alloc" "$(printf '(begin (define p (c-alloc 64)) (c-opaque? p))')" "#t"
run_test "ffi:opaque-not" "$(printf '(c-opaque? 42)')" "#f"
run_test "ffi:alloc-free" "$(printf '(begin (define p (c-alloc 1024)) (c-free p) (c-opaque? p))')" "#t"


# --emit-binary: standalone native binary
run_emit_test() {
    local name="$1"
    local input="$2"
    local expected="$3"
    local bin_path="/tmp/aura_emit_${name}"
    local result
    printf '%s' "$input" | timeout 10 "$AURA" --emit-binary "$bin_path" 2>/dev/null >/dev/null
    if [ -x "$bin_path" ]; then
        result=$("$bin_path" 2>&1 | tr -d '\n')
        if [ "$result" = "$expected" ]; then
            green "$name"
            PASS=$((PASS + 1))
        else
            red "$name"
            echo "       expected: $expected"
            echo "       got:      $result"
            FAIL=$((FAIL + 1))
        fi
        rm -f "$bin_path" "${bin_path}.o" "${bin_path}.tmp.aura" "${bin_path}.runtime.o" "${bin_path}.ir" 2>/dev/null
    else
        red "$name (no binary)"
        FAIL=$((FAIL + 1))
    fi
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
run_emit_test "emit:pair?"   "(pair? (cons 1 2))" "#t"
run_emit_test "emit:not-pair?"   "(pair? 42)" "#f"
run_emit_test "emit:null?"   "(null? 0)" "#t"
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
run_emit_test "emit:bin-search" "(import \"std/algorithm\")(binary-search 2 (list 1 2 3))" "#t"

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
run_test "agent:set-code-query" "$(printf '(set-code "(define (f x) (+ x 1))")(query:find \"f\")')" "(5)"

# set-code + query:node-type
run_test "agent:query-node-type" "$(printf '(set-code "(define (f x) (+ x 1))")(query:node-type \"Define\")')" "(5)"

# set-code + query:calls
run_test "agent:query-calls" "$(printf '(set-code "(define (f x) (+ x 1)) (define (g x) (f x))")(query:calls \"f\")')" "(8)"

# Pipe mode: display side-effect + return value (newline returns void, not printed)
run_test "agent:pipe-mode" "$(printf '(display 42)')" "42"

# EDSL mutate: rebind function
run_test "agent:mutate-rebind" "$(printf '(set-code "(define (add x y) (+ x y))")(query:find \"add\")(mutate:rebind \"add\" \"(define (add x y) (* x y))\")(eval-current)(add 1 2)')" "2"

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
