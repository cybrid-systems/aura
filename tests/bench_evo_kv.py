#!/usr/bin/env python3
"""
evo-kv style benchmark for Shape-based Speculative JIT (#53).

Measures throughput of generic JIT vs shape-specialized JIT
on hot-path operations. Uses --serve mode so function calls
share the same CompilerService instance (shape profiler persists).

Usage:
    python3 tests/bench_evo_kv.py                    # Run all benchmarks
    python3 tests/bench_evo_kv.py --quick            # Quick test (fewer warmup)
"""

import subprocess
import sys
import time
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
AURA = str(_SCRIPT_DIR.parent / "build" / "aura")

# ── Benchmark cases ───────────────────────────────────────────
# Each case: (name, code_to_define, call_template, warmup_count, measure_count)
# code_to_define is evaluated once, then call_template is evaluated N times

CASES = [
    ("int_add",     "(define (add x) (+ x 1))",      "(add {})",  150, 100),
    ("int_mul",     "(define (mul x y) (* x y))",   "(mul 99 {})", 150, 100),
    ("pair_car",    "(define (get-car p) (car p))",  "(get-car (cons {} 0))", 150, 100),
    ("pair_cdr",    "(define (get-cdr p) (cdr p))",  "(get-cdr (cons 0 {}))", 150, 100),
    ("fib_20",      "(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))", "(fib 20)", 50, 10),
    ("vec_ref",     "(define (vref v i) (vector-ref v i))", "(vref (vector 10 20 30 40 50 60 70 80 90 100) {})", 150, 100),
]


def bench_case(name, define_code, call_tmpl, warmup, measure) -> dict:
    """Run a single benchmark case via --serve."""
    inputs = [define_code]
    inputs += [call_tmpl.format(i) for i in range(warmup)]
    inputs += [call_tmpl.format(i + warmup) for i in range(measure)]
    input_str = "\n".join(inputs) + "\n"

    p = subprocess.Popen(
        [AURA, '--serve'],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    start = time.perf_counter()
    stdout, stderr = p.communicate(input_str, timeout=60)
    elapsed = time.perf_counter() - start

    spec_msgs = [l for l in stderr.split('\n') if 'spec:' in l and 'L1 for' in l]
    jit_fails = sum(1 for _ in stderr.split('\n') if 'addIRModule' in _)

    return {
        'name': name,
        'total_ms': elapsed * 1000,
        'total_calls': 1 + warmup + measure,
        'per_call_ms': (elapsed * 1000) / (1 + warmup + measure) if (1 + warmup + measure) > 0 else 0,
        'spec_count': len(spec_msgs),
        'jit_fails': jit_fails,
        'warmup': warmup,
        'measure': measure,
    }


def run_all(quick=False):
    results = []
    print("=" * 65)
    print("  evo-kv Speculative JIT Benchmark Suite  (#53)")
    print("=" * 65)
    print(f"  Binary: {AURA}")
    print()

    cases = CASES[:3] if quick else CASES

    for name, define, tmpl, warmup, measure in cases:
        print(f"  [{name:15s}] warmup={warmup} measure={measure} ...", end=" ", flush=True)
        r = bench_case(name, define, tmpl, warmup, measure)
        results.append(r)
        print(f"  {r['per_call_ms']:.3f}ms/call  spec={r['spec_count']}  "
              f"{'JITfail' if r['jit_fails'] else ''}")

    print()
    print("-" * 65)
    print(f"  {'Name':15s}  {'ms/call':>8s}  {'Spec':>5s}  {'JIT':>5s}")
    print("-" * 65)
    for r in results:
        jit_ok = "✓" if r['jit_fails'] == 0 else f"✗{r['jit_fails']}"
        print(f"  {r['name']:15s}  {r['per_call_ms']:>7.3f}ms  {r['spec_count']:>4d}  {jit_ok:>5s}")
    print("-" * 65)
    print()

    return results


if __name__ == "__main__":
    run_all(quick="--quick" in sys.argv)
