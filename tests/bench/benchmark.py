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
    python3 tests/benchmark.py --update --rationale "why"  # Update baseline + changelog (#1936)
    python3 tests/benchmark.py --strict --tolerance 5 --runs 3   # relative % + median-of-N

Issue #1569: hard SLO gate for CI. When strict mode is on, any regression
beyond ratio + absolute-delta thresholds exits with code 1.

Issue #1936: statistical / relative modes:
  - --mode relative (default): fail when delta > min_delta AND
    (cur/base > (1+tol/100) OR cur/base ≥ catastrophic). Absolute floor always applies.
  - --mode exact: tiny ratio band (legacy exact-ish; still uses absolute floor).
  - --runs N: measure each case N times and compare the **median** time (noise).
  - --tolerance / --tolerance-percent: relative allowance (default 20 ≈ legacy 1.2×).
  - Per-case overrides in benchmark_meta.json (tolerance_percent, min_delta_ms).
  - Warm-up process before the suite to avoid cold-start on case 1.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import re
import statistics
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

# Issue #1932: this file lives under tests/bench/ (was tests/).
# Ensure tests/python (fixture_store) and tests/bench are importable when
# invoked directly (not only via tests/benchmark.py thin entry or build.py).
_SCRIPT_DIR = Path(__file__).resolve().parent  # tests/bench
_REPO = _SCRIPT_DIR.parent.parent  # repo root
for _p in (_SCRIPT_DIR, _SCRIPT_DIR.parent / "python"):
    _s = str(_p)
    if _s not in sys.path:
        sys.path.insert(0, _s)

from benchmark_cases import BenchCase, load_benchmark_cases  # noqa: E402

# Use AURA_BIN env var if set (CI), otherwise default to <repo>/build/aura.
AURA = os.environ.get("AURA_BIN") or str(_REPO / "build" / "aura")
BASELINE_FILE = _SCRIPT_DIR / "benchmark_baseline.json"
META_FILE = _SCRIPT_DIR / "benchmark_meta.json"
UPDATES_LOG = _SCRIPT_DIR / "benchmark_updates.md"

# ── SLO thresholds (Issue #1569 / #1936) ──────────────────────
# Relative mode: ratio_threshold = 1 + tolerance_percent/100.
# Absolute floor avoids flaky sub-10ms cold-start noise while still
# catching real SLO hits. Catastrophic ratio always fails.
DEFAULT_TOLERANCE_PERCENT = 20.0  # 20% ↔ legacy 1.2× (#1569)
DEFAULT_REGRESSION_RATIO = 1.0 + DEFAULT_TOLERANCE_PERCENT / 100.0  # 1.2
DEFAULT_REGRESSION_MIN_DELTA_S = 0.020  # 20ms absolute floor
DEFAULT_CATASTROPHIC_RATIO = 3.0  # always fail if ≥3× regardless of delta
DEFAULT_IMPROVEMENT_RATIO = 0.7
DEFAULT_RUNS = 1
DEFAULT_STATISTICAL_RUNS = 3  # when --statistical without --runs


def env_flag_true(name: str) -> bool:
    return os.environ.get(name, "0").strip() in ("1", "true", "TRUE", "yes", "YES")


def is_strict_mode(argv: list[str] | None = None) -> bool:
    """True when --strict / --check or AURA_CI_STRICT_BENCH=1."""
    av = argv if argv is not None else sys.argv
    if env_flag_true("AURA_CI_STRICT_BENCH"):
        return True
    return "--strict" in av or "--check" in av


def load_meta() -> dict:
    """Optional per-benchmark metadata (#1936)."""
    if not META_FILE.is_file():
        return {"schema": 1936, "defaults": {}, "cases": {}}
    try:
        return json.loads(META_FILE.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {"schema": 1936, "defaults": {}, "cases": {}}


def case_thresholds(
    name: str,
    *,
    tolerance_percent: float,
    min_delta_s: float,
    catastrophic_ratio: float,
    meta: dict | None = None,
) -> tuple[float, float, float]:
    """Return (ratio_threshold, min_delta_s, catastrophic_ratio) for *name*."""
    meta = meta if meta is not None else load_meta()
    defaults = meta.get("defaults") or {}
    cases = meta.get("cases") or {}
    entry = cases.get(name) or {}
    tol = float(entry.get("tolerance_percent", defaults.get("tolerance_percent", tolerance_percent)))
    md_ms = entry.get("min_delta_ms", defaults.get("min_delta_ms"))
    md = float(md_ms) / 1000.0 if md_ms is not None else min_delta_s
    cat = float(entry.get("catastrophic_ratio", defaults.get("catastrophic_ratio", catastrophic_ratio)))
    return 1.0 + tol / 100.0, md, cat


def median_time(samples: list[float]) -> float:
    if not samples:
        return 0.0
    return float(statistics.median(samples))


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


def run_all(*, runs: int = 1) -> BenchSuiteResult:
    """Run the full suite. *runs* > 1 → median-of-N timing (#1936)."""
    runs = max(1, int(runs))
    suite = BenchSuiteResult(timestamp=get_timestamp())
    suite.total_cases = len(load_benchmark_cases())

    print(f"Aura Benchmark Suite — {suite.total_cases} cases")
    print(f"Binary: {AURA}")
    if runs > 1:
        print(f"Runs:   {runs} per case (median time_s)  [#1936 statistical]")
    print(f"{'─' * 60}")

    # Warm the aura binary / OS page cache so case 1 is not a cold-start
    # outlier under --strict (single-run CI). Discarded from results.
    with contextlib.suppress(Exception):
        run_aura("1", None)

    for i, bench in enumerate(load_benchmark_cases(), 1):
        samples: list[float] = []
        result: dict = {}
        for _ in range(runs):
            result = measure_pipeline(bench.name, bench.code, bench.pipeline)
            samples.append(float(result["time_s"]))
        result["time_samples"] = [round(s, 6) for s in samples]
        result["time_s"] = round(median_time(samples), 6)
        result["time_min_s"] = round(min(samples), 6)
        result["time_max_s"] = round(max(samples), 6)

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
        if runs > 1:
            time_str += f" (med/{runs})"
        print(f"  {bar} [{i:2d}/{suite.total_cases}] {bench.name:30s} {time_str:>14s}  {status}")
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
    os.makedirs(os.path.dirname(BASELINE_FILE) or ".", exist_ok=True)
    with open(BASELINE_FILE, "w") as f:
        json.dump(asdict(suite), f, indent=2)
    print(f"Baseline saved to {BASELINE_FILE}")


def append_update_log(*, rationale: str, suite: BenchSuiteResult, argv: list[str]) -> None:
    """Append a baseline-update entry to benchmark_updates.md (#1936)."""
    import datetime

    ts = datetime.datetime.now().astimezone().isoformat()
    block = (
        f"\n## {ts}\n\n"
        f"- **Rationale:** {rationale.strip()}\n"
        f"- **Cases:** {suite.total_cases} (passed={suite.passed}, failed={suite.failed})\n"
        f"- **Total time_s (median suite sum):** {suite.total_time_s}\n"
        f"- **Command:** `{' '.join(argv)}`\n"
    )
    header = (
        "# Benchmark baseline updates\n\n"
        "Log of intentional baseline refreshes (Issue #1936).\n"
        "Each `--update` with `--rationale` appends an entry here.\n"
    )
    if not UPDATES_LOG.is_file():
        UPDATES_LOG.write_text(header + block, encoding="utf-8")
    else:
        with open(UPDATES_LOG, "a", encoding="utf-8") as f:
            f.write(block)
    print(f"Update log appended: {UPDATES_LOG}")


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
        print('Run: python3 tests/bench/benchmark.py --update --rationale "…"')
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

    A case is a regression when absolute delta exceeds *min_delta_s* and either:
      - ratio ≥ catastrophic_ratio  (tagged catastrophic), OR
      - ratio > ratio_threshold

    Absolute floor is required even for catastrophic ratios (#1936): micro
    cases (5ms→18ms ≈ 3.6×) are environment noise, while real hits like
    20ms→400ms still fail (Δ ≫ floor).
    """
    if base_time_s <= 0:
        return None
    ratio = cur_time_s / base_time_s
    delta = cur_time_s - base_time_s
    if delta <= min_delta_s:
        return None
    catastrophic = ratio >= catastrophic_ratio
    if catastrophic or ratio > ratio_threshold:
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
    tolerance_percent: float | None = None,
    mode: str = "relative",
    use_meta: bool = True,
) -> tuple[list[RegressionHit], list[tuple[str, float, float, float]]]:
    """Compare suite vs baseline; return (regressions, improvements).

    mode:
      - relative: ratio = 1 + tol/100 (default), plus absolute floor
      - exact: ratio band ≈ 1.001 (still uses absolute floor for micro noise)
    """
    if baseline is None:
        baseline = load_baseline()
    bmap = {c["name"]: c for c in baseline.cases}
    cmap = {c["name"]: c for c in suite.cases}
    meta = load_meta() if use_meta else {"defaults": {}, "cases": {}}

    if mode == "exact":
        global_tol = 0.1  # 0.1%
    elif tolerance_percent is not None:
        global_tol = float(tolerance_percent)
    else:
        global_tol = (ratio_threshold - 1.0) * 100.0

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
        thr, md, cat = case_thresholds(
            name,
            tolerance_percent=global_tol,
            min_delta_s=min_delta_s,
            catastrophic_ratio=catastrophic_ratio,
            meta=meta,
        )
        hit = classify_regression(
            base_time,
            cur_time,
            ratio_threshold=thr,
            min_delta_s=md,
            catastrophic_ratio=cat,
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
    tolerance_percent: float | None = None,
    mode: str = "relative",
) -> bool:
    """Compare current results against stored baseline.

    Returns False when regressions found (for --check/--strict).
    `strict` only changes messaging (SLO VIOLATION vs warn).
    """
    if not validate_baseline_names(suite):
        return False

    thr = ratio_threshold if ratio_threshold is not None else DEFAULT_REGRESSION_RATIO
    tol = tolerance_percent if tolerance_percent is not None else (thr - 1.0) * 100.0
    regressions, improvements = collect_regressions(
        suite,
        ratio_threshold=thr,
        min_delta_s=min_delta_s,
        catastrophic_ratio=catastrophic_ratio,
        tolerance_percent=tol,
        mode=mode,
    )

    if regressions:
        label = "SLO VIOLATION" if strict else "REGRESSIONS"
        print(
            f"{'❌' if strict else '⚠️ '}  {label} "
            f"(mode={mode}, tol≈{tol:.1f}%, floor>{min_delta_s * 1000:.0f}ms, "
            f"or ≥{catastrophic_ratio}×):"
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


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        description="Aura compiler benchmark suite (#1569 / #1936)",
    )
    ap.add_argument("--json", action="store_true", help="emit suite JSON")
    ap.add_argument("--update", action="store_true", help="write baseline JSON")
    ap.add_argument(
        "--rationale",
        default="",
        help="required with --update: why the baseline is changing (#1936)",
    )
    ap.add_argument("--check", action="store_true", help="SLO gate (exit 1 on regression)")
    ap.add_argument("--strict", action="store_true", help="alias of --check + CI messaging")
    ap.add_argument(
        "--mode",
        choices=("relative", "exact"),
        default="relative",
        help="comparison mode (default: relative) (#1936)",
    )
    ap.add_argument(
        "--tolerance",
        "--tolerance-percent",
        dest="tolerance",
        type=float,
        default=None,
        help="relative tolerance percent (default 20 ≈ legacy 1.2×; try 5 for tighter)",
    )
    ap.add_argument(
        "--runs",
        type=int,
        default=None,
        help="repeat each case N times; use median time_s (#1936)",
    )
    ap.add_argument(
        "--statistical",
        action="store_true",
        help=f"shorthand for --runs {DEFAULT_STATISTICAL_RUNS} (median-of-N)",
    )
    ap.add_argument(
        "--min-delta-ms",
        type=float,
        default=DEFAULT_REGRESSION_MIN_DELTA_S * 1000.0,
        help="absolute delta floor in ms (default 20)",
    )
    ap.add_argument(
        "--catastrophic-ratio",
        type=float,
        default=DEFAULT_CATASTROPHIC_RATIO,
        help="always-fail ratio (default 3.0)",
    )
    return ap


def main(argv: list[str] | None = None) -> int:
    raw = list(sys.argv[1:] if argv is None else argv)
    # Allow legacy bare flags mixed with argparse
    ap = build_parser()
    args, _unknown = ap.parse_known_args(raw)

    strict = bool(args.strict or args.check or env_flag_true("AURA_CI_STRICT_BENCH"))
    mode = "run"
    if args.json:
        mode = "json"
    elif args.update:
        mode = "update"
    elif strict:
        mode = "check"

    runs = args.runs
    if runs is None:
        runs = DEFAULT_STATISTICAL_RUNS if args.statistical else DEFAULT_RUNS
    # CI strict defaults to median-of-3 unless user pinned --runs
    if strict and args.runs is None and not args.statistical:
        # Keep default runs=1 for speed on full 55-case suite in CI unless
        # AURA_BENCH_RUNS is set. Statistical opt-in via --statistical / env.
        env_runs = os.environ.get("AURA_BENCH_RUNS", "").strip()
        if env_runs.isdigit():
            runs = max(1, int(env_runs))

    tol = args.tolerance
    if tol is None:
        tol = DEFAULT_TOLERANCE_PERCENT
    if args.mode == "exact":
        tol = 0.1

    min_delta_s = float(args.min_delta_ms) / 1000.0
    cat = float(args.catastrophic_ratio)
    ratio_thr = 1.0 + float(tol) / 100.0

    # #1936: validate --update rationale *before* the full suite so a missing
    # rationale fails fast (exit 2) without burning wall-clock on 55 cases.
    rationale = ""
    if mode == "update":
        rationale = (args.rationale or "").strip()
        if not rationale:
            # Also accept env for non-interactive CI rebaseline
            rationale = os.environ.get("AURA_BENCH_RATIONALE", "").strip()
        if not rationale:
            print(
                '❌ --update requires --rationale "…" (or AURA_BENCH_RATIONALE) '
                "so baseline changes stay explainable (#1936).",
                file=sys.stderr,
            )
            return 2

    print(
        f"SLO config: mode={args.mode} tolerance={tol}% "
        f"(ratio>{ratio_thr:.3f}×) min_delta={min_delta_s * 1000:.0f}ms "
        f"catastrophic≥{cat}× runs={runs}"
    )

    suite = run_all(runs=runs)

    if mode == "json":
        print(json.dumps(asdict(suite), indent=2))
        return 0 if suite.failed == 0 else 1

    if mode == "update":
        save_baseline(suite)
        append_update_log(
            rationale=rationale,
            suite=suite,
            argv=["benchmark.py", *raw],
        )
        return 0 if suite.failed == 0 else 1

    if mode == "check":
        ok_reg = check_regression(
            suite,
            strict=True,
            ratio_threshold=ratio_thr,
            min_delta_s=min_delta_s,
            catastrophic_ratio=cat,
            tolerance_percent=tol,
            mode=args.mode,
        )
        if not ok_reg:
            print("\n❌ Benchmark SLO gate FAILED (AURA_CI_STRICT_BENCH / --strict / --check).")
            return 1
        if suite.failed > 0:
            print(f"\n❌ {suite.failed} functional benchmark case(s) failed.")
            return 1
        print("\n✅ Benchmark SLO gate clean.")
        return 0

    # Default run: warn on regression, do not hard-fail performance
    check_regression(
        suite,
        strict=False,
        ratio_threshold=ratio_thr,
        min_delta_s=min_delta_s,
        catastrophic_ratio=cat,
        tolerance_percent=tol,
        mode=args.mode,
    )
    return 0 if suite.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
