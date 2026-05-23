#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
KEYS="$HOME/code/keys"

# 运行 edsl benchmark 的辅助函数
run_model() {
    local label=$1
    local model=$2
    local base_url=$3
    local key_file=$4
    local log_file="$DIR/bench_results/${label}-$(date +%H%M).log"

    echo "=========================================="
    echo "  ▶ ${label}: ${model}"
    echo "  URL: ${base_url}"
    echo "  Log: ${log_file}"
    echo "=========================================="

    LLM_MODEL="${model}" \
    LLM_BASE_URL="${base_url}" \
    LLM_API_KEY="$(cat "$KEYS/$key_file")" \
    python3 "$DIR/tests/edsl_benchmark.py" \
        --max-attempts 3 \
        --json 2>&1 | tee "$log_file"

    local exit_code=${PIPESTATUS[0]}
    if [ $exit_code -ne 0 ]; then
        echo "⚠️  ${label} 退出码 ${exit_code}"
    fi
    echo ""
    return $exit_code
}

mkdir -p "$DIR/bench_results"

# Grok 4.3 — 最快 (~17min)
run_model "grok-4.3" \
    "grok-4.3-latest" \
    "https://api.x.ai/v1" \
    "grok"

# MiniMax M2.7 (~29min)
run_model "minimax-m2.7" \
    "minimax-m2.7" \
    "https://api.minimax.chat/v1" \
    "minimax"

# DeepSeek v4 Flash — 最慢 (~54min)
run_model "deepseek-v4-flash" \
    "deepseek-v4-flash" \
    "https://api.deepseek.com/v1" \
    "deepseek"

echo "=========================================="
echo "  全部完成"
echo "  Logs: $DIR/bench_results/"
echo "=========================================="
