#!/bin/bash
cd /home/dev/code/aura
export LLM_API_KEY="$(python3 -c "print(open('/home/dev/code/keys/deepseek').read().strip().split()[-1])")"
export LLM_BASE_URL="https://api.deepseek.com/v1"
export LLM_MODEL="deepseek-v4-flash"
timeout 1800 python3 -u tests/edsl_benchmark.py --max-attempts 5 > /tmp/bench_ds3.log 2>&1
