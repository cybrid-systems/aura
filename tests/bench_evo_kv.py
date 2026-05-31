#!/usr/bin/env python3
"""
evo-kv style benchmark for Shape-based Speculative JIT (#53).

Measures throughput of generic JIT vs shape-specialized JIT
on hot-path hash/key-value operations.

Usage:
    python3 tests/bench_evo_kv.py                    # Run all benchmarks
    python3 tests/bench_evo_kv.py --compare          # Compare JIT vs spec-JIT
    python3 tests/bench_evo_kv.py --warmup=N         # Warm for N iterations before measuring
"""

import os
import subprocess
import sys
import time
import json
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
AURA = os.environ.get("AURA_BIN") or str(_SCRIPT_DIR.parent / "build" / "aura")

# ── evo-kv operation suite ────────────────────────────────────
# Each case is (name, aura_code, warmup_count)
# warmup: number of eval cycles before measuring (to trigger shape profiling)

CASES = [
    # Pure fixnum addition (small recursion)
    ("int_add_short", '''
(letrec ((loop (lambda (i acc)
    (if (= i 0) acc
        (loop (- i 1) (+ acc 1))))))
  (loop 2000 0))
''', 200),

    # Fixnum multiplication
    ("int_mul_fact", '''
(letrec ((fact (lambda (n)
    (if (= n 0) 1 (* n (fact (- n 1)))))))
  (fact 15))
''', 200),

    # Pair car/cdr (L2 specialization target)
    ("pair_car_cdr", '''
(letrec ((run (lambda (i acc)
    (if (= i 0) acc
        (run (- i 1) (car (cons (cdr (cons acc acc)) acc)))))))
  (run 2000 42))
''', 200),

    # Simple list sum
    ("list_sum", '''
(letrec ((sum (lambda (lst)
    (if (null? lst) 0
        (+ (car lst) (sum (cdr lst)))))))
  (sum \'(1 2 3 4 5 6 7 8 9 10 11 12 13 14 15)))
''', 100),

    # Vector ref
    ("vec_ref", '''
(let ((v (vector 10 20 30 40 50 60 70 80 90 100)))
  (letrec ((sum (lambda (i acc)
      (if (= i 10) acc
          (sum (+ i 1) (+ acc (vector-ref v i)))))))
    (sum 0 0)))
''', 100),

    # Fibonacci small (benchmark standard)
    ("fib_20", '''
(letrec ((fib (lambda (n)
    (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))))
  (fib 20))
''', 100),

    ("fib_25", '''
(letrec ((fib (lambda (n)
    (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))))
  (fib 25))
''', 100),
]


def run_aura(code: str, args: list = None, timeout: int = 30) -> tuple:
    """Run aura and return (stdout, stderr, elapsed_seconds)."""
    cmd = [AURA]
    if args:
        cmd.extend(args)
    start = time.perf_counter()
    proc = subprocess.Popen(
        cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True,
    )
    stdout, stderr = proc.communicate(code + "\n", timeout=timeout)
    elapsed = time.perf_counter() - start
    return stdout.strip(), stderr.strip(), elapsed


def warmup(code: str, count: int) -> float:
    """Run code `count` times with JIT to warm shape profiles + cache."""
    start = time.perf_counter()
    for _ in range(count):
        stdout, stderr, _ = run_aura(code, None)
        if "error" in stderr:
            return -1.0
    return time.perf_counter() - start


def measure_jit(code: str, label: str, warmup_count: int) -> dict:
    """Measure throughput with JIT enabled. Returns timing dict."""
    result = {"label": label, "warmup_count": warmup_count}

    # 1. Warm up with --ir (builds shape profiles)
    if warmup_count > 0:
        warmup_time = warmup(code, warmup_count)
        if warmup_time < 0:
            result["error"] = "warmup failed"
            return result
        result["warmup_s"] = round(warmup_time, 4)

    # 2. Cold JIT (no shape profile → generic JIT)
    stdout, stderr, cold_time = run_aura(code)
    result["jit_cold_s"] = round(cold_time, 6)
    result["jit_cold_output"] = stdout

    # 3. Warm JIT (shape profile is warm from --ir warmup)
    stdout, stderr, warm_time = run_aura(code)
    result["jit_warm_s"] = round(warm_time, 6)
    result["jit_warm_output"] = stdout

    if "error" in stderr:
        result["error"] = stderr

    return result


def run_all() -> list:
    """Run all benchmark cases."""
    print("=" * 70)
    print("  evo-kv Speculative JIT Benchmark Suite  (#53)")
    print("=" * 70)
    print(f"  Binary: {AURA}")
    print(f"  Cases:  {len(CASES)}")
    print()

    all_results = []
    for name, code, warmup_count in CASES:
        print(f"  [{name:20s}] warmup={warmup_count}...", end=" ", flush=True)

        result = measure_jit(code, name, warmup_count)

        if "error" in result:
            print(f"ERROR: {result['error']}")
        else:
            cold = result["jit_cold_s"]
            warm = result["jit_warm_s"]
            if cold > 0:
                ratio = warm / cold if cold > 0 else 0
                print(f"cold={cold*1000:.2f}ms  warm={warm*1000:.2f}ms  "
                      f"ratio={ratio:.3f}x")
            else:
                print(f"cold={cold*1000:.2f}ms  warm={warm*1000:.2f}ms")

        all_results.append(result)

    # Summary
    print()
    print("-" * 70)
    print(f"  {'Name':20s}  {'Cold(ms)':>10s}  {'Warm(ms)':>10s}  {'Ratio':>8s}")
    print("-" * 70)
    for r in all_results:
        if "error" in r:
            print(f"  {r['label']:20s}  {'ERROR':>10s}")
        else:
            cold_ms = r["jit_cold_s"] * 1000
            warm_ms = r["jit_warm_s"] * 1000
            ratio = warm_ms / cold_ms if cold_ms > 0 else 0
            arrow = " ↑" if ratio > 1.05 else (" ↓" if ratio < 0.95 else " →")
            print(f"  {r['label']:20s}  {cold_ms:>8.2f}ms  {warm_ms:>8.2f}ms  "
                  f"{ratio:>5.3f}x{arrow}")
    print("-" * 70)
    print()

    return all_results


def compare_modes() -> None:
    """Compare IRInterpreter vs JIT vs spec-JIT throughput."""
    print("=" * 70)
    print("  Mode comparison: IRInterpreter vs JIT")
    print("=" * 70)

    for name, code, warmup_count in CASES[:3]:  # Just first 3 for speed
        print(f"\n  --- {name} ---")

        # IRInterpreter
        _, _, ir_time = run_aura(code, ["--ir"])
        print(f"    IRInterpreter: {ir_time*1000:.2f}ms")

        # JIT cold
        _, _, jit_time = run_aura(code)
        print(f"    JIT (cold):    {jit_time*1000:.2f}ms  "
              f"({ir_time/jit_time:.2f}x vs IR)")

        # JIT warm
        warmup(code, warmup_count)
        _, _, jit_warm = run_aura(code)
        print(f"    JIT (warm):    {jit_warm*1000:.2f}ms  "
              f"({ir_time/jit_warm:.2f}x vs IR, "
              f"{jit_time/jit_warm:.2f}x vs JIT cold)")


if __name__ == "__main__":
    if "--compare" in sys.argv:
        compare_modes()
    else:
        run_all()
