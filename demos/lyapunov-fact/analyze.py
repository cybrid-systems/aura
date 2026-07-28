#!/usr/bin/env python3
"""Aggregate trajectory CSVs → comparison table (Aura vs Python × rules/llm × filter)."""

from __future__ import annotations

import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
RESULTS = HERE / "results"


def _read(path: Path) -> list[dict]:
    if not path.exists():
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def _f(v: str | None, default: float = 0.0) -> float:
    try:
        return float(v) if v not in (None, "") else default
    except ValueError:
        return default


def _truthy(v: str | None) -> bool:
    return str(v).lower() in ("1", "true", "t", "yes")


def _classify(path: Path) -> tuple[str, str, str]:
    name = path.stem
    engine = name.split("_")[0]
    controller = "rules" if "rules" in name else ("llm" if "llm" in name else "?")
    filt = "on" if "filter-on" in name else ("off" if "filter-off" in name else "?")
    return engine, controller, filt


def _steps_to_S(records: list[dict]) -> int | None:
    for r in records:
        if _truthy(r.get("in_S")):
            try:
                return int(r.get("step", "-1"))
            except ValueError:
                return None
    return None


def _max_drawdown(vs: list[float]) -> float:
    if len(vs) < 2:
        return 0.0
    return min(vs[i + 1] - vs[i] for i in range(len(vs) - 1))


def _oscillations(vs: list[float], threshold: float = 0.5) -> int:
    n = 0
    prev: int | None = None
    for i in range(len(vs) - 1):
        d = vs[i + 1] - vs[i]
        if abs(d) <= threshold:
            continue
        sign = 1 if d > 0 else -1
        if prev is not None and sign != prev:
            n += 1
        prev = sign
    return n


def trial_stats(records: list[dict]) -> dict:
    if not records:
        return {
            "success": False,
            "k": None,
            "reject_rate": 0.0,
            "drawdown": 0.0,
            "osc": 0,
            "final_V": float("nan"),
        }
    vs = [_f(r.get("V")) for r in records]
    rejected = [_truthy(r.get("rejected")) for r in records]
    k = _steps_to_S(records)
    return {
        "success": k is not None,
        "k": k,
        "reject_rate": (sum(rejected) / len(rejected)) if rejected else 0.0,
        "drawdown": _max_drawdown(vs),
        "osc": _oscillations(vs),
        "final_V": vs[-1],
    }


def main() -> int:
    if not RESULTS.exists():
        print(f"!! results dir not found: {RESULTS}")
        return 1

    groups: dict[tuple[str, str, str], list[Path]] = defaultdict(list)
    for p in sorted(RESULTS.glob("*.csv")):
        groups[_classify(p)].append(p)
    if not groups:
        print("!! no CSVs in results/. Run exp1/exp2 first.")
        return 1

    header = (
        f"{'config':<34} {'n':>4} {'succ%':>7} {'mean_k':>8} {'med_k':>6} "
        f"{'rej%':>6} {'min_Δ':>8} {'osc':>5} {'mean_V∞':>8}"
    )
    print(header)
    print("-" * len(header))

    for key in sorted(groups):
        engine, controller, filt = key
        paths = groups[key]
        stats = [trial_stats(_read(p)) for p in paths]
        n = len(stats)
        succ = sum(1 for s in stats if s["success"])
        ks = [s["k"] for s in stats if s["k"] is not None]
        rejs = [s["reject_rate"] for s in stats]
        dds = [s["drawdown"] for s in stats]
        oscs = [s["osc"] for s in stats]
        fvs = [s["final_V"] for s in stats if s["final_V"] == s["final_V"]]
        label = f"{engine}|{controller}|filt-{filt}"
        print(
            f"{label:<34} {n:>4} {100 * succ / n:>6.1f}% "
            f"{(statistics.fmean(ks) if ks else float('nan')):>8.2f} "
            f"{(statistics.median(ks) if ks else float('nan')):>6.1f} "
            f"{100 * statistics.fmean(rejs) if rejs else 0:>5.1f}% "
            f"{(min(dds) if dds else 0):>8.3f} "
            f"{(statistics.fmean(oscs) if oscs else 0):>5.2f} "
            f"{(statistics.fmean(fvs) if fvs else float('nan')):>8.3f}"
        )

    print()
    print("Legend:")
    print("  config   = engine | controller | filter")
    print("  n        = independent trials")
    print("  succ%    = trials that reached S")
    print("  mean_k   = mean steps to first S")
    print("  med_k    = median steps to first S")
    print("  rej%     = mean per-step reject rate")
    print("  min_Δ    = worst single-step ΔV (drawdown)")
    print("  osc      = mean |ΔV|>0.5 sign-reversal count")
    print("  mean_V∞  = mean final V")
    print()
    print("S = test_fail==0 ∧ recursive_residual==0 ∧ node_count ≤ 1.5·initial")
    print("V = 10·test_fail + 2·recursive + 0.1·node_count + 0.05·|Δnodes|")
    return 0


if __name__ == "__main__":
    sys.exit(main())
