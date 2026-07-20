#!/usr/bin/env python3
"""
Aura Compiler Benchmark Suite

Establishes baselines for the AI mutation loop.
Measures parse time, IR pipeline time, execution time, and memory.

Usage:
    python3 tests/benchmark.py                  # Run all, print report (warn on regression)
    python3 tests/benchmark.py --json           # JSON output (AI-friendly)
    python3 tests/benchmark.py --check          # Compare with stored baseline (exit 1 on regression)
    python3 tests/benchmark.py --strict         # Same as --check; also set by AURA_CI_STRICT_BENCH=1
    python3 tests/benchmark.py --update         # Update stored baseline

Issue #1569: hard SLO gate for CI. When strict mode is on, any regression
beyond ratio + absolute-delta thresholds exits with code 1.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

from benchmark_cases import BenchCase, load_benchmark_cases

# Issue #1932: this file lives under tests/bench/ (was tests/).
# Use AURA_BIN env var if set (CI), otherwise default to <repo>/build/aura.
_SCRIPT_DIR = Path(__file__).resolve().parent  # tests/bench
_REPO = _SCRIPT_DIR.parent.parent  # repo root
AURA = os.environ.get("AURA_BIN") or str(_REPO / "build" / "aura")
BASELINE_FILE = _SCRIPT_DIR / "benchmark_baseline.json"

# ── SLO thresholds (Issue #1569) ──────────────────────────────
# Ratio-only is too noisy for sub-10ms cold-start cases; require BOTH
# ratio and absolute delta, OR a catastrophic ratio alone.
DEFAULT_REGRESSION_RATIO = 1.2  # issue AC: 1.2×
DEFAULT_REGRESSION_MIN_DELTA_S = 0.020  # 20ms absolute floor
DEFAULT_CATASTROPHIC_RATIO = 3.0  # always fail if ≥3× regardless of delta
DEFAULT_IMPROVEMENT_RATIO = 0.7


def env_flag_true(name: str) -> bool:
    return os.environ.get(name, "0").strip() in ("1", "true", "TRUE", "yes", "YES")


def is_strict_mode(argv: list[str] | None = None) -> bool:
    """True when --strict / --check or AURA_CI_STRICT_BENCH=1."""
    av = argv if argv is not None else sys.argv
    if env_flag_true("AURA_CI_STRICT_BENCH"):
        return True
    return "--strict" in av or "--check" in av


# ── Measurement helpers ───────────────────────────────────────


def run_aura(code: str, args: list[str] | None = None) -> tuple[str, str, float]:
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
    args = args_dict.get(pipeline)

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
        m = re.search(r"type:\s*(.+)", out)
        if m:
            inferred = m.group(1)
            if bench.expected_type and bench.expected_type in inferred:
                return True
    return False


def check_eval_result(bench: BenchCase, result: dict) -> bool:
    """Check eval/ir output against expected numeric value."""
    out = result.get("stdout", "")
    if "error:" in result.get("stderr", ""):
        if "coercion" in result.get("stderr", ""):
            pass
        else:
            return False
    if "parse error" in out:
        return False
    if bench.expected_val is None:
        return len(out) > 0
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


@dataclass
class RegressionHit:
    name: str
    base_time_s: float
    cur_time_s: float
    ratio: float
    delta_s: float
    catastrophic: bool = False

    def as_tuple(self) -> tuple:
        """Legacy (name, base, cur, ratio) for callers."""
        return (self.name, self.base_time_s, self.cur_time_s, self.ratio)


def get_timestamp() -> str:
    import datetime

    return datetime.datetime.now().astimezone().isoformat()


def run_all() -> BenchSuiteResult:
    suite = BenchSuiteResult(timestamp=get_timestamp())
    suite.total_cases = len(load_benchmark_cases())

    print(f"Aura Benchmark Suite — {suite.total_cases} cases")
    print(f"Binary: {AURA}")
    print(f"{'─' * 60}")

    for i, bench in enumerate(load_benchmark_cases(), 1):
        result = measure_pipeline(bench.name, bench.code, bench.pipeline)

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
        time_str = f"{result['time_s'] * 1000:.1f}ms"
        print(f"  {bar} [{i:2d}/{suite.total_cases}] {bench.name:30s} {time_str:>8s}  {status}")
        suite.cases.append(result)

    suite.total_time_s = sum(c["time_s"] for c in suite.cases)
    suite.total_time_s = round(suite.total_time_s, 3)

    print(f"{'─' * 60}")
    print(
        f"  Total: {suite.total_cases} cases, {suite.passed} passed, {suite.failed} failed, {suite.total_time_s:.2f}s"
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


def validate_baseline_names(suite: BenchSuiteResult) -> bool:
    """Ensure baseline case names match the benchmark fixture set."""
    baseline = load_baseline()
    if not baseline.cases:
        print("No baseline found. Run with --update first.")
        return False

    fixture_names = {c.name for c in load_benchmark_cases()}
    baseline_names = {c["name"] for c in baseline.cases}
    missing = sorted(fixture_names - baseline_names)
    extra = sorted(baseline_names - fixture_names)

    ok = True
    if missing:
        ok = False
        print(f"⚠️  BASELINE OUT OF SYNC: {len(missing)} fixture case(s) missing from baseline:")
        for name in missing:
            print(f"  + {name}")
    if extra:
        ok = False
        print(f"⚠️  BASELINE OUT OF SYNC: {len(extra)} stale baseline case(s) not in fixture:")
        for name in extra:
            print(f"  - {name}")
    if baseline.total_cases != len(fixture_names):
        ok = False
        print(f"⚠️  BASELINE total_cases={baseline.total_cases} but fixture has {len(fixture_names)} cases")
    if not ok:
        print("Run: python3 tests/benchmark.py --update")
    return ok


def classify_regression(
    base_time_s: float,
    cur_time_s: float,
    *,
    ratio_threshold: float = DEFAULT_REGRESSION_RATIO,
    min_delta_s: float = DEFAULT_REGRESSION_MIN_DELTA_S,
    catastrophic_ratio: float = DEFAULT_CATASTROPHIC_RATIO,
) -> RegressionHit | None:
    """Pure classifier for unit tests + gate.

    A case is a regression when:
      - ratio > catastrophic_ratio  (always), OR
      - ratio > ratio_threshold AND absolute delta > min_delta_s

    Absolute floor avoids flaky sub-10ms cold-start noise while still
    catching real SLO hits (e.g. 20ms → 400ms orchestration cases).
    """
    if base_time_s <= 0:
        return None
    ratio = cur_time_s / base_time_s
    delta = cur_time_s - base_time_s
    catastrophic = ratio >= catastrophic_ratio
    if catastrophic or (ratio > ratio_threshold and delta > min_delta_s):
        return RegressionHit(
            name="",
            base_time_s=base_time_s,
            cur_time_s=cur_time_s,
            ratio=ratio,
            delta_s=delta,
            catastrophic=catastrophic,
        )
    return None


def collect_regressions(
    suite: BenchSuiteResult,
    baseline: BenchSuiteResult | None = None,
    *,
    ratio_threshold: float = DEFAULT_REGRESSION_RATIO,
    min_delta_s: float = DEFAULT_REGRESSION_MIN_DELTA_S,
    catastrophic_ratio: float = DEFAULT_CATASTROPHIC_RATIO,
    improvement_ratio: float = DEFAULT_IMPROVEMENT_RATIO,
) -> tuple[list[RegressionHit], list[tuple[str, float, float, float]]]:
    """Compare suite vs baseline; return (regressions, improvements)."""
    if baseline is None:
        baseline = load_baseline()
    bmap = {c["name"]: c for c in baseline.cases}
    cmap = {c["name"]: c for c in suite.cases}

    regressions: list[RegressionHit] = []
    improvements: list[tuple[str, float, float, float]] = []

    for name, cur in cmap.items():
        if name not in bmap:
            continue
        base = bmap[name]
        base_time = float(base["time_s"])
        cur_time = float(cur["time_s"])
        if base_time <= 0:
            continue
        hit = classify_regression(
            base_time,
            cur_time,
            ratio_threshold=ratio_threshold,
            min_delta_s=min_delta_s,
            catastrophic_ratio=catastrophic_ratio,
        )
        if hit is not None:
            hit.name = name
            regressions.append(hit)
        else:
            ratio = cur_time / base_time
            if ratio < improvement_ratio:
                improvements.append((name, base_time, cur_time, ratio))

    return regressions, improvements


def check_regression(
    suite: BenchSuiteResult,
    *,
    strict: bool = False,
    ratio_threshold: float | None = None,
    min_delta_s: float = DEFAULT_REGRESSION_MIN_DELTA_S,
    catastrophic_ratio: float = DEFAULT_CATASTROPHIC_RATIO,
) -> bool:
    """Compare current results against stored baseline.

    Returns True when no SLO violations. In non-strict mode, still
    prints warnings but returns True if only soft regressions exist
    (caller may ignore). In strict mode, any regression → False.

    Actually: always returns False when regressions found (for --check).
    `strict` only changes messaging (SLO VIOLATION vs warn).
    """
    if not validate_baseline_names(suite):
        return False

    thr = ratio_threshold if ratio_threshold is not None else DEFAULT_REGRESSION_RATIO
    regressions, improvements = collect_regressions(
        suite,
        ratio_threshold=thr,
        min_delta_s=min_delta_s,
        catastrophic_ratio=catastrophic_ratio,
    )

    if regressions:
        label = "SLO VIOLATION" if strict else "REGRESSIONS"
        print(
            f"{'❌' if strict else '⚠️ '}  {label} (>{thr}× and >{min_delta_s * 1000:.0f}ms, or ≥{catastrophic_ratio}×):"
        )
        for hit in regressions:
            tag = " CATASTROPHIC" if hit.catastrophic else ""
            print(
                f"  {hit.name:30s} {hit.base_time_s * 1000:.1f}ms → "
                f"{hit.cur_time_s * 1000:.1f}ms ({hit.ratio:.1f}× Δ={hit.delta_s * 1000:.1f}ms){tag}"
            )
            if strict:
                print(f"SLO VIOLATION: {hit.name} regression {hit.ratio:.2f}x (delta={hit.delta_s * 1000:.1f}ms)")
    if improvements:
        print("✅ IMPROVEMENTS (<0.7× faster):")
        for name, base, cur, ratio in improvements:
            print(f"  {name:30s} {base * 1000:.1f}ms → {cur * 1000:.1f}ms ({ratio:.1f}×)")

    return len(regressions) == 0


# ── CLI entry point ───────────────────────────────────────────


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv if argv is None else argv)
    mode = "run"
    if "--json" in argv:
        mode = "json"
    elif "--update" in argv:
        mode = "update"
    elif "--check" in argv or "--strict" in argv or env_flag_true("AURA_CI_STRICT_BENCH"):
        mode = "check"

    strict = is_strict_mode(argv)

    suite = run_all()

    if mode == "json":
        print(json.dumps(asdict(suite), indent=2))
        return 0 if suite.failed == 0 else 1

    if mode == "update":
        save_baseline(suite)
        return 0 if suite.failed == 0 else 1

    if mode == "check":
        ok_reg = check_regression(suite, strict=True)
        if not ok_reg:
            if strict:
                print("\n❌ Benchmark SLO gate FAILED (AURA_CI_STRICT_BENCH / --strict / --check).")
            return 1
        if suite.failed > 0:
            print(f"\n❌ {suite.failed} functional benchmark case(s) failed.")
            return 1
        print("\n✅ Benchmark SLO gate clean.")
        return 0

    # Default run: warn on regression, do not hard-fail performance
    # unless strict env is set (already handled as mode=check above).
    check_regression(suite, strict=False)
    return 0 if suite.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
