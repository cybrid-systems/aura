#!/bin/bash
# tests/run_parallel.sh — Run bench.aura tasks in parallel
#
# Splits 135 tasks across N workers using BENCH_OFFSET/BENCH_LIMIT.
# Each worker starts a separate Aura process.
#
# Usage:
#   LLM_API_KEY="***" bash tests/run_parallel.sh [workers]
#   LLM_API_KEY="***" bash tests/run_parallel.sh 4

set -e
cd "$(dirname "$0")/.."

WORKERS="${1:-4}"
TOTAL=135
BATCH=$(( (TOTAL + WORKERS - 1) / WORKERS ))
AURA="${AURA:-./build/aura}"
TIMEOUT=1200  # 20 min per worker (was 600 — 10 min insufficient for 14+ tasks at ~1 min/task)

echo "=== Parallel bench.aura ==="
echo "Workers: $WORKERS"
echo "Total tasks: $TOTAL"
echo "Batch size: $BATCH"
echo ""

# Start workers in background
PIDS=()
OUTDIR=$(mktemp -d /tmp/aura_bench_parallel_XXXX)
for ((i=0; i<WORKERS; i++)); do
    OFFSET=$((i * BATCH))
    REMAIN=$((TOTAL - OFFSET))
    if [ "$REMAIN" -le 0 ]; then break; fi
    LIMIT=$(( BATCH < REMAIN ? BATCH : REMAIN ))
    OUT="$OUTDIR/worker_${i}.txt"
    echo "Worker $i: offset=$OFFSET limit=$LIMIT → $OUT"
    LLM_API_KEY="$LLM_API_KEY" \
      LLM_MODEL="${LLM_MODEL:-deepseek-v4-flash}" \
      LLM_BASE_URL="${LLM_BASE_URL:-https://api.deepseek.com/v1}" \
      BENCH_OFFSET=$OFFSET BENCH_LIMIT=$LIMIT \
      timeout $TIMEOUT "$AURA" < tests/bench.aura > "$OUT" 2>&1 &
    PIDS+=($!)
done

# Wait for all workers
echo ""
echo "Waiting for ${#PIDS[@]} workers..."
FAILED=0
for pid in "${PIDS[@]}"; do
    wait "$pid" || ((FAILED++))
done

echo ""
echo "=== Results ==="
TOTAL_PASS=0
TOTAL_FAIL=0
for ((i=0; i<WORKERS; i++)); do
    OUT="$OUTDIR/worker_${i}.txt"
    if [ -f "$OUT" ]; then
        PASS=$(grep -c " OK" "$OUT" 2>/dev/null || echo 0)
        FAIL=$(grep -c " FAIL" "$OUT" 2>/dev/null || echo 0)
        echo "Worker $i: $PASS pass / $FAIL fail"
        TOTAL_PASS=$((TOTAL_PASS + PASS))
        TOTAL_FAIL=$((TOTAL_FAIL + FAIL))
    fi
done

TOTAL=$((TOTAL_PASS + TOTAL_FAIL))
echo "---"
echo "Total: $TOTAL_PASS pass / $TOTAL_FAIL fail ($(( TOTAL > 0 ? TOTAL_PASS * 100 / TOTAL : 0 ))%)"
echo "Logs: $OUTDIR"