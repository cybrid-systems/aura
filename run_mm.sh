#!/bin/bash
cd /home/dev/code/aura
export LLM_API_KEY="$(python3 -c "print(open('/home/dev/code/keys/minmax-token-plan').read().strip())")"
export LLM_BASE_URL="https://api.minimaxi.com/v1"
export LLM_MODEL="minimax-m2.7"
timeout 3000 python3 -u tests/edsl_benchmark.py --max-attempts 5 > /tmp/bench_mm2.log 2>&1
