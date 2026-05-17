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
run_test "stdlib:pi"     "$(printf '(import \"std/math\") pi')" "3.141592653589793"

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
echo "=== Error Handling Tests ==="

run_test "error:t-alive"    "$(printf '(try (+ 1 2) (catch (e) 0))')" "3"
run_test "error:t-caught"   "$(printf '(try (error \"x\") (catch (e) 42))')" "42"
run_test "error:car"        "$(printf '(try (car 42) (catch (e) \"ok\"))')" "\"ok\""
run_test "error:mod0"       "$(printf '(try (modulo 5 0) (catch (e) \"ok\"))')" "\"ok\""

echo ""
echo "============"
printf "Tests: %d passed, %d failed\n" "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
