#!/usr/bin/env python3
"""Experiment 1: Python — rule controller + external ΔV filter.

Same V, S, rules, threshold as the Aura runner. Execution model:
whole-function string replace + deepcopy-style external filter.
"""

from __future__ import annotations

import argparse
import ast
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT))

from common import (  # noqa: E402
    FILTER_DELTA_V,
    TEST_VALUE,
    StepRecord,
    V_value,
    energy_of,
    in_S,
    save_trajectory,
)

INITIAL_CODE = """\
def fact(n):
    if n == 0:
        return 1
    return n * fact(n - 1)
"""

GOOD_ITER = """\
def fact(n):
    acc = 1
    while n > 0:
        acc *= n
        n -= 1
    return acc
"""


def _exec_fact(code: str, n: int = 10) -> int | None:
    ns: dict = {}
    try:
        exec(compile(code, "<trial>", "exec"), ns, ns)
    except Exception:
        return None
    fn = ns.get("fact")
    if not callable(fn):
        return None
    try:
        return int(fn(n))
    except Exception:
        return None


def test_fail(code: str) -> int:
    return 0 if _exec_fact(code) == TEST_VALUE else 1


def recursive_residual(code: str) -> int:
    # Self-call count minus the def line's fact( name.
    return max(0, code.count("fact(") - 1)


def node_count(code: str) -> float:
    try:
        tree = ast.parse(code)
    except SyntaxError:
        return float("inf")
    return float(sum(1 for _ in ast.walk(tree)))


def measure(code: str, energy: float) -> tuple[int, int, float, float]:
    tf = test_fail(code)
    rr = recursive_residual(code)
    nc = node_count(code)
    return tf, rr, nc, V_value(tf, rr, nc, energy)


def propose_action(code: str) -> tuple[str | None, str]:
    tf, rr, nc, _ = measure(code, 0.0)
    if tf == 1:
        return GOOD_ITER, "fix-correctness"
    if rr > 0:
        return GOOD_ITER, "remove-recursion"
    if nc > 25:
        return GOOD_ITER, "simplify"
    return None, "stop"


def run_trial(use_filter: bool, max_steps: int = 15) -> list[StepRecord]:
    code = INITIAL_CODE
    energy = 0.0
    initial_nc = node_count(code)
    records: list[StepRecord] = []

    tf, rr, nc, v = measure(code, energy)
    records.append(
        StepRecord(
            step=0,
            V=v,
            test_fail=tf,
            recursive_residual=rr,
            node_count=nc,
            energy=energy,
            rejected=False,
            in_S=in_S(tf, rr, nc, initial_nc),
            action="init",
        )
    )

    for step in range(1, max_steps + 1):
        new_code, action = propose_action(code)
        if new_code is None:
            tf, rr, nc, v = measure(code, energy)
            records.append(
                StepRecord(
                    step=step,
                    V=v,
                    test_fail=tf,
                    recursive_residual=rr,
                    node_count=nc,
                    energy=energy,
                    rejected=False,
                    in_S=in_S(tf, rr, nc, initial_nc),
                    action=action,
                )
            )
            break

        old_v = V_value(tf, rr, nc, energy)
        cand_energy = energy_of(nc, node_count(new_code))
        ctf, crr, cnc, cand_v = measure(new_code, cand_energy)
        delta = cand_v - old_v
        rejected = use_filter and delta > FILTER_DELTA_V

        if not rejected:
            code = new_code
            energy = cand_energy
            tf, rr, nc, v = ctf, crr, cnc, cand_v
        else:
            v = old_v

        records.append(
            StepRecord(
                step=step,
                V=v,
                test_fail=tf,
                recursive_residual=rr,
                node_count=nc,
                energy=energy,
                rejected=rejected,
                in_S=in_S(tf, rr, nc, initial_nc),
                action=action,
                extra=f"delta={delta:.3f}",
            )
        )
        if in_S(tf, rr, nc, initial_nc):
            break
    return records


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--filter", choices=["on", "off"], default="on")
    parser.add_argument("--trials", type=int, default=20)
    parser.add_argument("--max-steps", type=int, default=15)
    args = parser.parse_args()

    use_filter = args.filter == "on"
    for trial in range(args.trials):
        traj = run_trial(use_filter=use_filter, max_steps=args.max_steps)
        save_trajectory(traj, f"python_rules_filter-{args.filter}_trial{trial:02d}")
        last = traj[-1] if traj else None
        print(
            f"python rules filter={args.filter} trial={trial:02d} "
            f"steps={len(traj)} last_V={last.V if last else float('nan'):.3f} "
            f"in_S={last.in_S if last else False}",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
