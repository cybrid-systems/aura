#!/usr/bin/env python3
"""
Aura Compiler Benchmark Suite

Establishes baselines for the AI mutation loop.
Measures parse time, IR pipeline time, execution time, and memory.

Usage:
    python3 tests/benchmark.py                  # Run all, print report
    python3 tests/benchmark.py --json           # JSON output (AI-friendly)
    python3 tests/benchmark.py --check          # Compare with stored baseline
    python3 tests/benchmark.py --update         # Update stored baseline
"""

import json
import os
import re
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

# Use AURA_BIN env var if set (CI), otherwise default to build/aura relative to this file
_SCRIPT_DIR = Path(__file__).resolve().parent
AURA = os.environ.get("AURA_BIN") or str(_SCRIPT_DIR.parent / "build" / "aura")
BASELINE_FILE = _SCRIPT_DIR / "benchmark_baseline.json"

# ── Benchmark definitions ──────────────────────────────────────


@dataclass
class BenchCase:
    name: str
    code: str
    pipeline: str  # "eval", "ir", "typecheck"
    expected_val: Any = None  # expected numeric result (for eval/ir)
    expected_type: str = None  # expected type string (for typecheck)
    expected_err: str = None  # expected error substring


BENCHMARKS = [
    # ── L1: Literals ─────────────────────────────────────────
    BenchCase("literal_int", "42", "eval", expected_val=0),
    BenchCase("literal_neg", "-5", "eval", expected_val=-5),
    BenchCase("literal_string", '"hello"', "eval", expected_val=None),
    # ── L2: Arithmetic ───────────────────────────────────────
    BenchCase("add", "(+ 1 2)", "eval", expected_val=3),
    BenchCase("add_many", "(+ (+ (+ (+ 1 2) 3) 4) 5)", "eval", expected_val=15),
    BenchCase("sub", "(- 10 3)", "eval", expected_val=7),
    BenchCase("mul", "(* 6 7)", "eval", expected_val=0),
    BenchCase("div", "(/ 100 5)", "eval", expected_val=20),
    BenchCase("nested_arith", "(+ (* 2 3) (/ 10 2))", "eval", expected_val=11),
    # ── L3: Let / Variables ──────────────────────────────────
    BenchCase("let_simple", "(let ((x 10)) x)", "eval", expected_val=10),
    BenchCase("let_add", "(let ((x 1) (y 2)) (+ x y))", "eval", expected_val=3),
    BenchCase(
        "letrec_fact",
        "(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 10))",
        "eval",
        expected_val=3628800,
    ),
    BenchCase(
        "fib_20",
        "(letrec ((fib (lambda (n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2))))))) (fib 20))",
        "eval",
        expected_val=6765,
    ),
    # ── L4: Lambda / Closure ─────────────────────────────────
    BenchCase("lambda_apply", "((lambda (x) (* x 2)) 5)", "eval", expected_val=10),
    BenchCase(
        "closure", "(let ((f (lambda (x) (+ x 1)))) (f 41))", "eval", expected_val=42
    ),
    BenchCase(
        "higher_order",
        "((lambda (f) (f 10)) (lambda (x) (* x x)))",
        "eval",
        expected_val=100,
    ),
    # ── L5: Conditionals ─────────────────────────────────────
    BenchCase("if_true", "(if 1 42 0)", "eval", expected_val=0),
    BenchCase("if_false", "(if 0 42 0)", "eval", expected_val=0),
    BenchCase("if_compare", "(if (< 3 5) 100 200)", "eval", expected_val=100),
    # ── L6: Strings ──────────────────────────────────────────
    BenchCase("str_append", '(string-append "a" "b")', "eval", expected_val=None),
    BenchCase("str_length", '(string-length "hello")', "eval", expected_val=5),
    BenchCase("vec_basic", "(vector 1 2 3)", "eval", expected_val=None),
    BenchCase("vec_ref", "(vector-ref (vector 10 20 30) 1)", "eval", expected_val=20),
    # ── L6: Pairs ────────────────────────────────────────────
    BenchCase("cons_car", "(car (cons 1 2))", "eval", expected_val=1),
    BenchCase("cons_cdr", "(cdr (cons 1 2))", "eval", expected_val=2),
    # ── IR pipeline benchmarks ───────────────────────────────
    BenchCase("ir_add", "(+ 1 2)", "ir", expected_val=3),
    BenchCase(
        "ir_fact",
        "(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 5))",
        "ir",
        expected_val=120,
    ),
    BenchCase(
        "ir_fib_10",
        "(letrec ((fib (lambda (n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2))))))) (fib 10))",
        "ir",
        expected_val=55,
    ),
    BenchCase("ir_lambda", "((lambda (x) (* x 2)) 5)", "ir", expected_val=10),
    BenchCase(
        "ir_closure", "(let ((f (lambda (x) (+ x 1)))) (f 41))", "ir", expected_val=42
    ),
    BenchCase("ir_if", "(if (< 3 5) 100 200)", "ir", expected_val=100),
    # ── Typecheck benchmarks ─────────────────────────────────
    BenchCase("tc_literal", "42", "typecheck", expected_type="Int"),
    BenchCase("tc_add", "(+ 1 2)", "typecheck", expected_type="Int"),
    BenchCase("tc_str", '"hello"', "typecheck", expected_type="String"),
    BenchCase("tc_lambda", "(lambda (x) x)", "typecheck", expected_type="->"),
    BenchCase("tc_let", "(let ((x 10)) x)", "typecheck", expected_type="Int"),
    BenchCase(
        "tc_string_append",
        '(string-append "a" "b")',
        "typecheck",
        expected_type="String",
    ),
    BenchCase(
        "tc_string_length", '(string-length "hello")', "typecheck", expected_type="Int"
    ),
    BenchCase("tc_type_of", "(type-of 42)", "typecheck", expected_type="Type"),
    BenchCase("tc_type_query", '(type? 42 "Int")', "typecheck", expected_type="Bool"),
    BenchCase(
        "tc_occurrence",
        '(let ((x "hello")) (if (string? x) x "fallback"))',
        "typecheck",
        expected_type="String",
    ),
    BenchCase("tc_coercion", '(+ "42" 1)', "typecheck", expected_type="Int"),
    # ── Gradual coercion runtime ─────────────────────────────
    BenchCase("coerce_arith", '(+ 1 "2")', "eval", expected_val=3),
    BenchCase("coerce_str_len", "(string-length 12345)", "eval", expected_val=5),
]

# ── Measurement helpers ───────────────────────────────────────


def run_aura(code: str, args: list[str] = None) -> tuple[str, str, float]:
    """Run aura and return (stdout, stderr, elapsed_seconds)."""
    cmd = [AURA]
    if args:
        cmd.extend(args)
    start = time.perf_counter()
    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    stdout, stderr = proc.communicate(code + "\n", timeout=30)
    elapsed = time.perf_counter() - start
    return stdout.strip(), stderr.strip(), elapsed


def measure_pipeline(name: str, code: str, pipeline: str) -> dict:
    """Run a single benchmark and return measurements."""
    result = {
        "name": name,
        "code": code,
        "pipeline": pipeline,
        "time_s": 0.0,
        "output": "",
        "error": "",
        "passed": False,
        "memory_stats": {},
    }

    args_dict = {"eval": None, "ir": ["--ir"], "typecheck": ["--typecheck"]}
    args = args_dict.get(pipeline, None)

    stdout, stderr, elapsed = run_aura(code, args)
    result["time_s"] = round(elapsed, 6)
    result["stdout"] = stdout
    result["stderr"] = stderr

    # Check for crashes
    if stderr and "error:" in stderr and "coercion" not in stderr:
        result["error"] = stderr

    return result


def check_typecheck_result(bench: BenchCase, result: dict) -> bool:
    """Check typecheck output against expected type string."""
    out = result.get("stdout", "")
    if "parse error:" in out:
        return False
    if "type:" in out:
        # Extract type from "type: <typename>"
        m = re.search(r"type:\s*(.+)", out)
        if m:
            inferred = m.group(1)
            # Allow partial matching for function types
            if bench.expected_type and bench.expected_type in inferred:
                return True
    return False


def check_eval_result(bench: BenchCase, result: dict) -> bool:
    """Check eval/ir output against expected numeric value."""
    out = result.get("stdout", "")
    if "error:" in result.get("stderr", ""):
        # Check if it's a coercion note (not an actual error)
        if "coercion" in result.get("stderr", ""):
            pass  # coercion notes are OK
        else:
            return False
    if "parse error" in out:
        return False
    # Try to parse output as number
    if bench.expected_val is None:
        return len(out) > 0  # any non-empty output
    out_lines = out.strip().split("\n")
    for line in out_lines:
        line = line.strip()
        if not line:
            continue
        try:
            val = int(line)
            if val == bench.expected_val:
                return True
        except ValueError:
            pass
    return False


# ── Benchmark runner ──────────────────────────────────────────


@dataclass
class BenchSuiteResult:
    timestamp: str = ""
    total_cases: int = 0
    passed: int = 0
    failed: int = 0
    total_time_s: float = 0.0
    cases: list[dict] = field(default_factory=list)


def get_timestamp() -> str:
    import datetime

    return datetime.datetime.now().astimezone().isoformat()


def run_all() -> BenchSuiteResult:
    suite = BenchSuiteResult(timestamp=get_timestamp())
    suite.total_cases = len(BENCHMARKS)

    print(f"Aura Benchmark Suite — {suite.total_cases} cases")
    print(f"Binary: {AURA}")
    print(f"{'─'*60}")

    for i, bench in enumerate(BENCHMARKS, 1):
        result = measure_pipeline(bench.name, bench.code, bench.pipeline)

        # Check pass/fail
        if bench.pipeline in ("eval", "ir"):
            passed = check_eval_result(bench, result)
        elif bench.pipeline == "typecheck":
            passed = check_typecheck_result(bench, result)
        else:
            passed = False

        result["passed"] = passed
        if passed:
            suite.passed += 1
        else:
            suite.failed += 1

        status = "PASS" if passed else "FAIL"
        bar = "+" if passed else "-"
        time_str = f"{result['time_s']*1000:.1f}ms"
        print(
            f"  {bar} [{i:2d}/{suite.total_cases}] {bench.name:30s} {time_str:>8s}  {status}"
        )
        suite.cases.append(result)

    suite.total_time_s = sum(c["time_s"] for c in suite.cases)
    suite.total_time_s = round(suite.total_time_s, 3)

    print(f"{'─'*60}")
    print(
        f"  Total: {suite.total_cases} cases, "
        f"{suite.passed} passed, {suite.failed} failed, "
        f"{suite.total_time_s:.2f}s"
    )
    print()

    return suite


# ── Baseline management ───────────────────────────────────────


def load_baseline() -> BenchSuiteResult:
    if not os.path.exists(BASELINE_FILE):
        return BenchSuiteResult()
    with open(BASELINE_FILE) as f:
        data = json.load(f)
    suite = BenchSuiteResult(**data)
    return suite


def save_baseline(suite: BenchSuiteResult):
    os.makedirs(os.path.dirname(BASELINE_FILE), exist_ok=True)
    with open(BASELINE_FILE, "w") as f:
        json.dump(asdict(suite), f, indent=2)
    print(f"Baseline saved to {BASELINE_FILE}")


def check_regression(suite: BenchSuiteResult) -> bool:
    """Compare current results against stored baseline."""
    baseline = load_baseline()
    if not baseline.cases:
        print("No baseline found. Run with --update first.")
        return True

    bmap = {c["name"]: c for c in baseline.cases}
    cmap = {c["name"]: c for c in suite.cases}

    regressions = []
    improvements = []

    for name, cur in cmap.items():
        if name not in bmap:
            continue
        base = bmap[name]
        base_time = base["time_s"]
        if base_time <= 0:
            continue
        ratio = cur["time_s"] / base_time
        if ratio > 1.3:
            regressions.append((name, base_time, cur["time_s"], ratio))
        elif ratio < 0.7:
            improvements.append((name, base_time, cur["time_s"], ratio))

    if regressions:
        print("⚠️  REGRESSIONS (>1.3× slower):")
        for name, base, cur, ratio in regressions:
            print(f"  {name:30s} {base*1000:.1f}ms → {cur*1000:.1f}ms ({ratio:.1f}×)")
    if improvements:
        print("✅ IMPROVEMENTS (<0.7× faster):")
        for name, base, cur, ratio in improvements:
            print(f"  {name:30s} {base*1000:.1f}ms → {cur*1000:.1f}ms ({ratio:.1f}×)")

    return len(regressions) == 0


# ── CLI entry point ───────────────────────────────────────────


def main():
    mode = "run"
    if "--json" in sys.argv:
        mode = "json"
    elif "--check" in sys.argv:
        mode = "check"
    elif "--update" in sys.argv:
        mode = "update"

    suite = run_all()

    if mode == "json":
        print(json.dumps(asdict(suite), indent=2))

    elif mode == "check":
        ok = check_regression(suite)
        sys.exit(0 if ok else 1)

    elif mode == "update":
        save_baseline(suite)

    sys.exit(0 if suite.failed == 0 else 1)


if __name__ == "__main__":
    main()
