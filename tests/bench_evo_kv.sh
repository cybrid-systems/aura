#!/bin/bash
# evo-kv Performance Benchmark
# Compares evo-kv-core.aura (baseline) vs evo-kv-core-perf.aura (performance-region)
# Usage: ./tests/bench_evo_kv.sh

set -e

AURA="${AURA:-./build/aura}"
BENCH_DIR="$(dirname "$0")"
PROJECT_DIR="$(cd "$BENCH_DIR/.." && pwd)"

if [ ! -f "$AURA" ]; then
    echo "ERROR: $AURA not found. Build first or set AURA env var."
    exit 1
fi

green() { printf "  \033[32m✓\033[0m %s\n" "$1"; }
red()   { printf "  \033[31m✗\033[0m %s\n" "$1"; }
blue()  { printf "\033[34m%s\033[0m\n" "$1"; }

echo "=== evo-kv Performance Benchmark ==="
echo "Binary: $AURA"
echo ""

# Build a benchmark input: runs N total operations in batches of BATCH_SIZE
# (limited by recursion depth)
make_set_bench() {
    local core_file="$1"
    local total_ops="${2:-150000}"
    local batch="${3:-1500}"
    local reps=$((total_ops / batch))
    cat <<AUBENCH
(begin
  (load "projects/evo-kv/${core_file}")
  (let* ((batch ${batch}) (reps ${reps}))
    (letrec ((run-batches (lambda (b)
            (if (< b reps)
              (begin
                (letrec ((go (lambda (i)
                        (if (< i batch)
                          (begin
                            (evo-set (string-append "k" (number->string i)) i)
                            (go (+ i 1)))
                          "done"))))
                  (go 0))
                (run-batches (+ b 1)))
              "done"))))
      (run-batches 0))))
AUBENCH
}

make_get_bench() {
    local core_file="$1"
    local n="${2:-50000}"
    local batch="${3:-1500}"
    local reps=$((n / batch))
    cat <<AUBENCH
(begin
  (load "projects/evo-kv/${core_file}")
  ;; pre-populate keys
  (letrec ((prep (lambda (i)
          (if (< i ${n})
            (begin (evo-set (string-append "k" (number->string i)) i) (prep (+ i 1)))
            "done"))))
    (prep 0))
  ;; benchmark gets
  (let* ((batch ${batch}) (reps ${reps}))
    (letrec ((run-batches (lambda (b)
            (if (< b reps)
              (begin
                (letrec ((go (lambda (i)
                        (if (< i batch)
                          (begin
                            (evo-get (string-append "k" (number->string i)))
                            (go (+ i 1)))
                          "done"))))
                  (go 0))
                (run-batches (+ b 1)))
              "done"))))
      (run-batches 0))))
AUBENCH
}

make_setgetdel_bench() {
    local core_file="$1"
    local total_ops="${2:-75000}"
    local batch="${3:-1500}"
    local reps=$((total_ops / batch))
    cat <<AUBENCH
(begin
  (load "projects/evo-kv/${core_file}")
  (let* ((batch ${batch}) (reps ${reps}))
    (letrec ((run-batches (lambda (b)
            (if (< b reps)
              (begin
                (letrec ((go (lambda (i)
                        (if (< i batch)
                          (begin
                            (evo-set (string-append "k" (number->string i)) i)
                            (evo-get (string-append "k" (number->string i)))
                            (evo-del (string-append "k" (number->string i)))
                            (go (+ i 1)))
                          "done"))))
                  (go 0))
                (run-batches (+ b 1)))
              "done"))))
      (run-batches 0))))
AUBENCH
}

# Run a single benchmark and return elapsed ms
bench() {
    local label="$1"
    local input="$2"
    local runs="${3:-3}"
    local total_ms=0
    local results=()

    for i in $(seq 1 $runs); do
        local start=$(date +%s%N)
        echo "$input" | timeout 60 "$AURA" 2>/dev/null >/dev/null
        local end=$(date +%s%N)
        local elapsed_ms=$(( (end - start) / 1000000 ))
        total_ms=$((total_ms + elapsed_ms))
        results+=("$elapsed_ms")
    done
    local avg=$((total_ms / runs))

    # Format results
    local result_str=$(printf "%d" "$avg")
    for r in "${results[@]}"; do
        result_str="$result_str / $r"
    done
    echo "$result_str"
}

echo ""
blue "=== 1. Set-only throughput (150k ops) ==="
echo ""
echo "  Baseline (no perf-region):"
baseline_set=$(bench "baseline-set" "$(make_set_bench "evo-kv-core.aura" 150000)")
echo "    avg / runs: $baseline_set ms"

echo "  Optimized (perf-region):"
perf_set=$(bench "perf-set" "$(make_set_bench "evo-kv-core-perf.aura" 150000)")
echo "    avg / runs: $perf_set ms"

echo ""
blue "=== 2. Get-only throughput (75k ops, warm store) ==="
echo ""
echo "  Baseline (no perf-region):"
baseline_get=$(bench "baseline-get" "$(make_get_bench "evo-kv-core.aura" 75000)")
echo "    avg / runs: $baseline_get ms"

echo "  Optimized (perf-region):"
perf_get=$(bench "perf-get" "$(make_get_bench "evo-kv-core-perf.aura" 75000)")
echo "    avg / runs: $perf_get ms"

echo ""
blue "=== 3. Set+Get+Del mixed throughput (75k ops) ==="
echo ""
echo "  Baseline (no perf-region):"
baseline_mix=$(bench "baseline-mix" "$(make_setgetdel_bench "evo-kv-core.aura" 75000)")
echo "    avg / runs: $baseline_mix ms"

echo "  Optimized (perf-region):"
perf_mix=$(bench "perf-mix" "$(make_setgetdel_bench "evo-kv-core-perf.aura" 75000)")
echo "    avg / runs: $perf_mix ms"

echo ""
echo "=== Summary ==="
echo ""
echo "  Workload            | Baseline (ms) | Perf-reg (ms) | Speedup"
echo "  --------------------|--------------:|--------------:|-------:"
# Parse averages (first number before the "/")
bl_set=$(echo "$baseline_set" | awk '{print $1}')
pf_set=$(echo "$perf_set" | awk '{print $1}')
bl_get=$(echo "$baseline_get" | awk '{print $1}')
pf_get=$(echo "$perf_get" | awk '{print $1}')
bl_mix=$(echo "$baseline_mix" | awk '{print $1}')
pf_mix=$(echo "$perf_mix" | awk '{print $1}')

calculate_speedup() {
    local b=$1 p=$2
    if [ "$p" != "0" ] && [ "$p" != "" ] && [ "$b" != "0" ] && [ "$b" != "" ]; then
        echo "scale=2; $b / $p" | bc 2>/dev/null || echo "N/A"
    else
        echo "N/A"
    fi
}

printf "  %-20s | %12s | %12s | %s\n" "Set"    "$bl_set" "$pf_set" "$(calculate_speedup $bl_set $pf_set)"
printf "  %-20s | %12s | %12s | %s\n" "Get"    "$bl_get" "$pf_get" "$(calculate_speedup $bl_get $pf_get)"
printf "  %-20s | %12s | %12s | %s\n" "Mix"    "$bl_mix" "$pf_mix" "$(calculate_speedup $bl_mix $pf_mix)"

echo ""
echo "=== Correctness ==="
echo ""
# Run correctness test for both versions
echo "  Baseline core tests:"
baseline_result=$(printf '(load "projects/evo-kv/evo-kv-core.aura") (load "projects/evo-kv/evo-kv-metrics.aura") (load "projects/evo-kv/evo-kv-evolve.aura") (load "projects/evo-kv/evo-kv-grok.aura") (load "projects/evo-kv/test-evo-kv.aura")\n' | timeout 30 "$AURA" 2>&1)
echo "    $(echo "$baseline_result" | grep -c '\[PASS\]') PASS / $(echo "$baseline_result" | grep -c '\[FAIL\]') FAIL"

echo "  Perf-region core tests:"
perf_result=$(printf '(load "projects/evo-kv/evo-kv-core-perf.aura") (load "projects/evo-kv/evo-kv-metrics.aura") (load "projects/evo-kv/evo-kv-evolve.aura") (load "projects/evo-kv/evo-kv-grok.aura") (load "projects/evo-kv/test-evo-kv.aura")\n' | timeout 30 "$AURA" 2>&1)
echo "    $(echo "$perf_result" | grep -c '\[PASS\]') PASS / $(echo "$perf_result" | grep -c '\[FAIL\]') FAIL"

echo ""
green "Benchmark complete."
