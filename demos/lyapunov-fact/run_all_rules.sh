#!/usr/bin/env bash
# Full rules comparison (Aura vs Python × filter on/off).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
cd "$REPO"

if [[ ! -x build/aura ]]; then
  echo "!! build/aura missing — run ./build.py build first" >&2
  exit 1
fi

export AURA_PIPELINE_STRICT=0
TRIALS="${TRIALS:-20}"
MAX_STEPS="${MAX_STEPS:-15}"

# Fresh results for rules only (keep llm CSVs if any)
rm -f demos/lyapunov-fact/results/aura_rules_*.csv \
      demos/lyapunov-fact/results/python_rules_*.csv

echo "=== Python rules (filter=off) ==="
python3 demos/lyapunov-fact/exp1_rules/run_python_rules.py --filter off --trials "$TRIALS" --max-steps "$MAX_STEPS"
echo "=== Python rules (filter=on) ==="
python3 demos/lyapunov-fact/exp1_rules/run_python_rules.py --filter on  --trials "$TRIALS" --max-steps "$MAX_STEPS"
echo "=== Aura rules (filter=off) ==="
python3 demos/lyapunov-fact/exp1_rules/run_aura_rules.py   --filter off --trials "$TRIALS" --max-steps "$MAX_STEPS"
echo "=== Aura rules (filter=on) ==="
python3 demos/lyapunov-fact/exp1_rules/run_aura_rules.py   --filter on  --trials "$TRIALS" --max-steps "$MAX_STEPS"

echo
echo "=== Summary ==="
python3 demos/lyapunov-fact/analyze.py
