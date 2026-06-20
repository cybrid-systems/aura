#!/usr/bin/env python3
"""run_bench_async.py — Run bench.aura under --serve-async for fiber parallelism.

Wraps bench.aura content as JSON-protocol exec commands to serve-async.
This enables fiber:spawn, async HTTP, and inter-fiber send/recv.

Usage:
  LLM_API_KEY="***" python3 tests/run_bench_async.py [workers]

Environment (same as bench.aura):
  LLM_MODEL         Model name (default: deepseek-v4-flash)
  LLM_BASE_URL      API base URL (default: https://api.deepseek.com/v1)
  BENCH_LIMIT       Max tasks (default: all)
  BENCH_OFFSET      Task offset (default: 0)
  BENCH_ROUNDS      Number of rounds (default: 1)
  BENCH_ATTEMPTS    Max attempts (default: 3)
  BENCH_WORKERS     Worker fibers (default: 4)
"""

import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AURA = os.environ.get("AURA", str(ROOT / "build" / "aura"))
MODEL = os.environ.get("LLM_MODEL", "deepseek-v4-flash")
BASE_URL = os.environ.get("LLM_BASE_URL", "https://api.deepseek.com/v1")
LIMIT = os.environ.get("BENCH_LIMIT", "0")
OFFSET = os.environ.get("BENCH_OFFSET", "0")
ROUNDS = os.environ.get("BENCH_ROUNDS", "1")
ATTEMPTS = os.environ.get("BENCH_ATTEMPTS", "3")
WORKERS = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("BENCH_WORKERS", "4")


def wrap_bench():
    """Read bench.aura, inject env overrides, wrap in serve-async exec command."""
    bench_path = ROOT / "tests" / "bench.aura"
    with open(bench_path) as f:
        code = f.read()

    # Prepend env setup (overrides the env var reads in bench.aura)
    preamble = f"""; Auto-generated preamble for serve-async mode
(define model "{MODEL}")
(define rounds {ROUNDS})
(define max-a {ATTEMPTS})
(define workers {WORKERS})
(define limit {LIMIT})
(define offset {OFFSET})

"""
    full_code = preamble + code

    # Build JSON exec command
    cmd = {
        "session": "default",
        "cmd": "exec",
        "code": full_code,
    }
    return json.dumps(cmd)


def main():
    bench_code = wrap_bench()

    env = os.environ.copy()
    env["AURA"] = AURA

    print("=== Aura Parallel Benchmark ===", flush=True)
    print(f"  Binary: {AURA}", flush=True)
    print("  Mode: --serve-async (fiber parallel)", flush=True)
    print(f"  Workers: {WORKERS}", flush=True)
    print(f"  Model: {MODEL}", flush=True)
    print(f"  Base URL: {BASE_URL}", flush=True)
    print(f"  Tasks: limit={LIMIT}, offset={OFFSET}", flush=True)
    print(f"  Rounds: {ROUNDS}, attempts: {ATTEMPTS}", flush=True)
    print("", flush=True)

    proc = subprocess.Popen(
        [AURA, "--serve-async"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
        text=True,
    )

    # Send the bench code
    out, _ = proc.communicate(input=bench_code + "\n")

    print(out)
    return 0 if "Passed" in out or "OK" in out else 1


if __name__ == "__main__":
    sys.exit(main())
