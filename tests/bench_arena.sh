#!/bin/bash
# Arena Benchmark Suite
# Compares performance with and without TL arena for various workloads.
# Usage: ./tests/bench_arena.sh [--build-first]

set -e

AURA="${AURA:-./build/aura}"
BUILD_DIR="./build"
PASS=0
FAIL=0

green() { printf "  \033[32m✓\033[0m %s\n" "$1"; }
red()   { printf "  \033[31m✗\033[0m %s\n" "$1"; }
blue()  { printf "\033[34m%s\033[0m\n" "$1"; }

# Parse flags
if [[ "$1" == "--build-first" ]]; then
    echo "Building Aura..."
    cd "$(dirname "$0")/.."
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release 2>/dev/null || true
    cmake --build build -j"$(nproc)" 2>&1 | tail -5
    cd - >/dev/null
fi

if [ ! -f "$AURA" ]; then
    echo "ERROR: $AURA not found. Build first or set AURA env var."
    exit 1
fi

echo "=== Aura Arena Benchmark Suite ==="
echo "Binary: $AURA"
echo ""

bench_cmd() {
    local label="$1"
    local input="$2"
    local result
    local elapsed

    # Warmup run
    echo "$input" | timeout 10 "$AURA" +RTS -N1 2>/dev/null >/dev/null || true

    # Timed runs
    local total=0
    local runs=3
    local last_out=""
    for i in $(seq 1 $runs); do
        local start=$(date +%s%N)
        last_out=$(echo "$input" | timeout 10 "$AURA" 2>/dev/null || echo "TIMEOUT")
        local end=$(date +%s%N)
        local diff=$(( (end - start) / 1000000 ))  # ms
        total=$((total + diff))
    done
    local avg=$((total / runs))
    printf "  %-30s %8d ms  (last out: %s)\n" "$label" "$avg" "$(echo "$last_out" | head -c 40)"
}

echo ""
blue "=== 1. Pair-cons benchmark ==="
echo "Creating 10000 cons cells and reading car"
bench_cmd "with-arena"  "(letrec ((go (lambda (i acc) (if (< i 0) (car acc) (go (- i 1) (cons i acc)))))) (go 10000 ()))"

echo ""
blue "=== 2. Map over iota (list creation) ==="
echo "Create 500-element list and car"
bench_cmd "with-arena"  "
(define (iota n)
  (letrec ((go (lambda (i acc) (if (< i 0) acc (go (- i 1) (cons i acc))))))
    (go (- n 1) ())))
(car (iota 500))"

echo ""
blue "=== 3. Closure heavy: make + call many closures ==="
echo "Make 1000 closures, call each once"
bench_cmd "with-arena"  "
(letrec ((mk (lambda (n)
   (if (< n 0) 'ok
     (begin ((lambda (x) (+ x 1)) n) (mk (- n 1)))))))
 (mk 1000))"

echo ""
blue "=== 4. Arena-offset tracking ==="
echo "Check that arena-offset returns a value"
result=$(echo "(arena-offset)" | timeout 5 "$AURA" 2>/dev/null)
echo "  arena-offset result: $result"

echo ""
blue "=== 5. Nested pair creation (stress test) ==="
echo "Create deeply nested pairs: (((...(1 2)...))) 100 deep"
bench_cmd "with-arena"  "
(letrec ((mk (lambda (n v)
   (if (< n 0) v (mk (- n 1) (cons (car v) (cdr v)))))))
 (mk 100 '(1 . 2)))"

echo ""
blue "=== 6. No-arena comparison ==="
echo "Run key benchmarks without arena flag"
bench_cmd "no-arena-pairs" "--no-arena
(letrec ((go (lambda (i acc) (if (< i 0) (car acc) (go (- i 1) (cons i acc)))))) (go 10000 ()))"

bench_cmd "no-arena-closures" "--no-arena
(letrec ((mk (lambda (n)
   (if (< n 0) 'ok
     (begin ((lambda (x) (+ x 1)) n) (mk (- n 1)))))))
 (mk 1000))"

echo ""
blue "=== 8. Performance-region comparison ==="
echo "Compare O2 vs O3 optimization with performance-region"

bench_cmd "pr-cons" "
(performance-region
  (letrec ((go (lambda (i acc) (if (< i 0) (car acc) (go (- i 1) (cons i acc))))))
    (go 10000 ())))"

bench_cmd "pr-closure" "
(performance-region
  (letrec ((mk (lambda (n)
     (if (< n 0) 'ok
       (begin ((lambda (x) (+ x 1)) n) (mk (- n 1)))))))
   (mk 1000)))"

bench_cmd "pr-ping" "
(performance-region
  (letrec ((lp (lambda (i c) (if (>= i 5000) c (lp (+ i 1) (+ c 1))))))
    (lp 0 0)))"

echo ""
echo ""
blue "=== 7. evo-kv test (if available) ==="
EVO_KV_TEST="$(dirname "$0")/../projects/evo-kv/test-evo-kv.aura"
if [ -f "$EVO_KV_TEST" ]; then
    echo "Running evo-kv correctness test (with pre-load)..."
    evo_input='(load "projects/evo-kv/evo-kv.aura") (load "projects/evo-kv/test-evo-kv.aura")'
    result=$(echo "$evo_input" | timeout 60 "$AURA" 2>&1)
    pass_count=$(echo "$result" | grep -c '\[PASS\]')
    fail_count=$(echo "$result" | grep -c '\[FAIL\]')
    echo "  $pass_count passed, $fail_count failed"
    green "evo-kv test completed"
else
    echo "  SKIP: $EVO_KV_TEST not found"
fi

echo ""
echo "=== Benchmark Summary ==="
echo "All benchmarks completed."

echo ""
echo "=== Benchmark Summary ==="
echo "All benchmarks completed."
