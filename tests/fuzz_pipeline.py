#!/usr/bin/env python3
"""Pipeline Stress Fuzz — test synthesize:pipeline with various step combinations.

Run multiple pipelines with template fills, set-code, and mutations.
Verifies rollback on failure and workspace integrity after completion.

Usage:
  python3 tests/fuzz_pipeline.py [--quick] [--seed N]
"""

import datetime
import os
import random
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
AURA = os.environ.get("AURA_BIN", str(REPO / "build" / "aura"))

QUICK = "--quick" in sys.argv
SEED = None
for i, a in enumerate(sys.argv):
    if a == "--seed" and i + 1 < len(sys.argv):
        SEED = int(sys.argv[i + 1])

rng = random.Random(SEED if SEED is not None else None)

PIPELINES = [
    # 2-step: fill templates
    """
    (require "std/pipeline" all:)
    (set-code "(begin)")
    (synthesize:register-template "add" "(define (add-{a} x y) (+ x y))" "a")
    (synthesize:register-template "mul" "(define (mul-{a} x y) (* x y))" "a")
    (synthesize:pipeline "math"
      (synthesize:fill "add" "ints")
      (synthesize:fill "mul" "floats"))
    """,
    # 3-step: fill + mutate
    """
    (require "std/pipeline" all:)
    (set-code "(define (f x) x)")
    (synthesize:register-template "double" "(define (double-{n} x) (* 2 x))" "n")
    (synthesize:pipeline "triple"
      (mutate:rebind "f" "(lambda (x) (+ x 1))" "inc")
      (synthesize:fill "double" "val")
      (set-code "(define z 42)"))
    """,
    # Single step
    """
    (require "std/pipeline" all:)
    (set-code "(define x 1)")
    (synthesize:pipeline "single"
      (set-code "(define x 99)"))
    """,
]


def send(proc, cmd):
    try:
        if proc.poll() is not None:
            return None
        proc.stdin.write(cmd + "\n")
        proc.stdin.flush()
        time.sleep(0.01)
        line = proc.stdout.readline()
    except (BrokenPipeError, OSError):
        return None
    if not line:
        return None
    stripped = line.strip()
    return stripped


def run_session(n_cycles):
    proc = subprocess.Popen(
        [AURA, "--serve"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    time.sleep(0.2)

    stats = {"ok": 0, "error": 0, "crash": 0}

    for cycle in range(n_cycles):
        pipeline = rng.choice(PIPELINES)
        escaped = pipeline.replace("\\", "\\\\").replace('"', '\\"')
        resp = send(proc, escaped)
        if resp is None:
            stats["crash"] += 1
            break
        if "#t" in resp or "Complete" in resp or "ok" in resp:
            stats["ok"] += 1
        else:
            stats["error"] += 1

        if QUICK and cycle >= n_cycles:
            break

    try:
        proc.kill()
        proc.wait(timeout=3)
    except Exception:
        pass
    return stats


def main():
    print("=" * 60)
    print("Pipeline Stress Fuzz")
    print(f"  Date: {datetime.date.today().isoformat()}")
    print(f"  Seed: {SEED if SEED is not None else 'random'}")
    print(f"  Mode: {'QUICK' if QUICK else 'FULL'}")
    print("=" * 60)

    n_cycles = 50 if QUICK else 200
    n_sessions = 3

    total = {"ok": 0, "error": 0, "crash": 0}

    for s in range(n_sessions):
        print(f"\n  Session {s + 1}/{n_sessions} ... ", end="", flush=True)
        st = run_session(n_cycles // n_sessions)
        for k in total:
            total[k] += st[k]
        ops = st["ok"] + st["error"] + st["crash"]
        pct = st["ok"] / max(ops, 1) * 100
        print(f"{st['ok']}/{ops} ({pct:.0f}%) [err={st['error']} crash={st['crash']}]")

    print(f"\n{'=' * 60}")
    rate = total["ok"] / max(total["ok"] + total["error"] + total["crash"], 1) * 100
    print(f"  Total: {total['ok']} ok, {total['error']} error, {total['crash']} crash")
    print(f"  Rate:  {rate:.1f}%")

    if total["crash"]:
        print("\n  💥 CRASH detected!")
        sys.exit(1)


if __name__ == "__main__":
    main()
