#!/usr/bin/env python3
"""Aggregate trajectory CSVs and report summary stats per configuration.

Reads every CSV under `demos/lyapunov-fact/results/`, classifies by
filename (aura/python × rules/llm × filter on/off), and prints one
summary line per config with: success rate, mean/median steps to
reach S, reject rate, V max drawdown, oscillation count.

No plotting dependency — the spec asks for "key comparison tables"
that can be eyeballed in a terminal. If a future iteration wants
PNG plots it can add `matplotlib`; the summary lines below are the
primary deliverable.
"""

from __future__ import annotations

import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
RESULTS = HERE / "results"


def _read_records(path: Path) -> list[dict]:
    if not path.exists():
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def _float(v: str | None, default: float = 0.0) -> float:
    if v is None or v == "":
        return default
    try:
        return float(v)
    except ValueError:
        return default


def _classify(path: Path) -> tuple[str, str, str]:
    """Return (engine, controller, filter) from the filename like
    `aura_rules_filter-on_trial00.csv`."""
    name = path.stem  # e.g. aura_rules_filter-on_trial00
    parts = name.split("_")
    engine = parts[0]  # aura | python
    if "rules" in parts:
        controller = "rules"
    elif "llm" in parts:
        controller = "llm"
    else:
        controller = "?"
    if "filter-on" in name:
        filt = "on"
    elif "filter-off" in name:
        filt = "off"
    else:
        filt = "?"
    return engine, controller, filt


def _steps_to_S(records: list[dict]) -> int | None:
    """First step at which in_S is True/1 — None if never reached."""
    for r in records:
        if r.get("in_S", "0") in ("1", "True", "true"):
            try:
                return int(r.get("step", "-1"))
            except ValueError:
                return None
    return None


def _max_drawdown(Vs: list[float]) -> float:
    """Largest single-step drop in V. Negative number."""
    if len(Vs) < 2:
        return 0.0
    drops = [Vs[i + 1] - Vs[i] for i in range(len(Vs) - 1)]
    return min(drops) if drops else 0.0


def _oscillations(Vs: list[float], threshold: float = 0.5) -> int:
    """Number of |ΔV| > threshold reversals. Each adjacent pair crossing
    the threshold in either direction counts once."""
    n = 0
    prev_sign: int | None = None  # -1 = decrease, +1 = increase
    for i in range(len(Vs) - 1):
        d = Vs[i + 1] - Vs[i]
        if abs(d) <= threshold:
            continue
        sign = 1 if d > 0 else -1
        if prev_sign is not None and sign != prev_sign:
            n += 1
        prev_sign = sign
    return n


def summarize(records: list[dict]) -> dict:
    if not records:
        return {
            "trials": 0,
            "success": 0,
            "rate": 0.0,
            "mean_steps": None,
            "median_steps": None,
            "reject_rate": 0.0,
            "max_drawdown": 0.0,
            "oscillations": 0.0,
        }
    Vs = [_float(r.get("V")) for r in records]
    rejected = [int(r.get("rejected", "0") in ("1", "True", "true")) for r in records]
    [r.get("in_S", "0") in ("1", "True", "true") for r in records]
    steps_to_S = [_steps_to_S([r]) for r in records]
    steps_to_S = [s for s in steps_to_S if s is not None]
    successes = sum(1 for s in steps_to_S)
    trials = len(set(r.get("step", "0") for r in records))  # unique steps ⇒ n trials (rough)
    # We group by file so `trials` is best taken as the number of files
    # we'll surface at the call site.
    return {
        "trials": 0,  # filled by caller
        "success": successes,
        "rate": successes / max(trials, 1),
        "mean_steps": statistics.fmean(steps_to_S) if steps_to_S else None,
        "median_steps": statistics.median(steps_to_S) if steps_to_S else None,
        "reject_rate": (sum(rejected) / len(rejected)) if rejected else 0.0,
        "max_drawdown": _max_drawdown(Vs),
        "oscillations": _oscillations(Vs),
    }


def main() -> int:
    if not RESULTS.exists():
        print(f"!! results dir not found: {RESULTS}. Run the experiments first.")
        return 1

    # group files by (engine, controller, filter)
    groups: dict[tuple[str, str, str], list[Path]] = defaultdict(list)
    for p in sorted(RESULTS.glob("*.csv")):
        engine, controller, filt = _classify(p)
        groups[(engine, controller, filt)].append(p)

    if not groups:
        print("!! no CSVs in results/. Run exp1 or exp2 first.")
        return 1

    # Print a per-config table
    header = f"{'config':<32} {'trials':>6} {'succ%':>7} {'mean_k':>8} {'med_k':>6} {'rej%':>6} {'min_Δ':>8} {'osc':>5}"
    print(header)
    print("-" * len(header))

    for (engine, controller, filt), paths in sorted(groups.items()):
        per_trial = [summarize(_read_records(p)) for p in paths]
        n = len(per_trial)
        if n == 0:
            continue
        # trial-level success rate
        succ_trials = sum(1 for s in per_trial if s["success"] >= 1)
        rate = succ_trials / n
        # mean over per-trial success_steps
        ms = [s["mean_steps"] for s in per_trial if s["mean_steps"] is not None]
        med = [s["median_steps"] for s in per_trial if s["median_steps"] is not None]
        rej = [s["reject_rate"] for s in per_trial]
        dd = [s["max_drawdown"] for s in per_trial]
        osc = [s["oscillations"] for s in per_trial]
        label = f"{engine:6}|{controller:5}|filt-{filt}"
        print(
            f"{label:<32} {n:>6} {rate * 100:>6.1f}% "
            f"{(statistics.fmean(ms) if ms else float('nan')):>8.2f} "
            f"{(statistics.median(med) if med else float('nan')):>6.1f} "
            f"{(statistics.fmean(rej) if rej else 0) * 100:>5.1f}% "
            f"{(min(dd) if dd else 0):>8.3f} "
            f"{(statistics.fmean(osc) if osc else 0):>5.2f}"
        )

    print()
    print("Legend:")
    print("  config = engine | controller | filter-mode")
    print("  trials = number of independent trials in results/")
    print("  succ%  = % of trials that reached S (test_fail==0, recursive==0, size<=1.3*initial)")
    print("  mean_k = mean number of accepted steps to first reach S (across trials)")
    print("  med_k  = median steps to first reach S")
    print("  rej%   = mean reject rate across trials")
    print("  min_Δ  = worst single-step ΔV (most negative = deepest drawdown)")
    print("  osc    = mean oscillation count (large |ΔV| sign reversals, threshold 0.5)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
