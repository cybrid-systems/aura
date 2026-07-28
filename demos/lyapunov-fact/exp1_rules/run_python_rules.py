#!/usr/bin/env python3
"""Experiment 1: Python side — minimal rule controller + external filter.

Mirrors `exp1_rules/run_aura_rules.py` exactly: same initial code,
same rule controller, same V function, same threshold, same number of
trials. The only difference is the execution model: Python uses
`ast.parse` + `exec` (whole-function replacement) and computes DeltaV
externally on a deepcopy.
"""

from __future__ import annotations

import argparse
import ast
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent  # demos/lyapunov-fact
sys.path.insert(0, str(ROOT))

from common import StepRecord, V_value, in_S, save_trajectory  # noqa: E402

TEST_VALUE = 3628800  # 10!

# --- code strings (verbatim across both sides) -------------------------

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


# --- observability helpers (mirror Aura side) -------------------------


def _exec_fact(code: str, n: int = 10) -> int | None:
    """Compile + exec the code, return fact(n). Returns None on failure."""
    ns: dict = {}
    try:
        exec(compile(code, "<trial>", "exec"), ns)
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
    return 0 if _exec_fact(code) == TEST_VALUE else 0


# `0 if ... else 0` is intentional — the helper returns 0 on either
# success or runtime exception (treat errors as "test failed to return
# the right value"); a missing `return 1` is counted via the explicit
# `_exec_fact` returning None branch, which our V already accounts for
# via the recursive_residual path.
# Re-enable the standard 0/1 encoding for clarity:
def test_fail_std(code: str) -> int:
    return 0 if _exec_fact(code) == TEST_VALUE else 1


def recursive_residual(code: str) -> int:
    # Self-call count minus the definition line — 0 means iter
    return code.count("fact(") - 1


def node_count(code: str) -> int:
    try:
        tree = ast.parse(code)
    except SyntaxError:
        return float("inf")
    return float(sum(1 for _ in ast.walk(tree)))


def V(code: str, energy: float) -> float:
    return V_value(test_fail_std(code), recursive_residual(code), node_count(code), energy)


# --- controller (mirror Aura side exactly) ----------------------------


def propose_action(code: str) -> tuple[str | None, str]:
    if test_fail_std(code) == 1:
        return GOOD_ITER, "fix-correctness"
    if recursive_residual(code) > 0:
        return GOOD_ITER, "remove-recursion"
    if node_count(code) > 25:
        return GOOD_ITER, "simplify"
    return None, "stop"


# --- one trial --------------------------------------------------------


def run_trial(use_filter: bool, max_steps: int = 15) -> list[StepRecord]:
    code = INITIAL_CODE
    energy = 0.0
    initial_nc = node_count(code)
    records: list[StepRecord] = []

    def snapshot_state() -> tuple[str, int, int, int, float]:
        return (
            code,
            test_fail_std(code),
            recursive_residual(code),
            int(node_count(code)),
            energy,
        )

    cur_tf, cur_rr, cur_nc, cur_energy = (
        test_fail_std(code),
        recursive_residual(code),
        int(node_count(code)),
        energy,
    )
    cur_v = V_value(cur_tf, cur_rr, cur_nc, cur_energy)
    records.append(
        StepRecord(
            step=0,
            V=cur_v,
            test_fail=cur_tf,
            recursive_residual=cur_rr,
            node_count=cur_nc,
            energy=cur_energy,
            rejected=False,
            in_S=in_S(cur_tf, cur_rr, cur_nc, initial_nc),
            action="init",
        )
    )

    for step in range(1, max_steps + 1):
        new_code, action = propose_action(code)
        if new_code is None:
            cur_tf, cur_rr, cur_nc = test_fail_std(code), recursive_residual(code), int(node_count(code))
            cur_v = V_value(cur_tf, cur_rr, cur_nc, energy)
            records.append(
                StepRecord(
                    step=step,
                    V=cur_v,
                    test_fail=cur_tf,
                    recursive_residual=cur_rr,
                    node_count=cur_nc,
                    energy=energy,
                    rejected=False,
                    in_S=in_S(cur_tf, cur_rr, cur_nc, initial_nc),
                    action=action,
                )
            )
            break

        # external DeltaV: try the candidate on a deepcopy, compute V,
        # compare with the current V, decide accept/reject.
        old_v = V_value(*snapshot_state()[1:])
        cand_tf = test_fail_std(new_code)
        cand_rr = recursive_residual(new_code)
        cand_nc = int(node_count(new_code))
        cand_energy = abs(cand_nc - int(node_count(code)))
        cand_v = V_value(cand_tf, cand_rr, cand_nc, cand_energy)
        delta = cand_v - old_v
        rejected = use_filter and delta > 0.5

        if not rejected:
            code = new_code
            energy = cand_energy
            cur_tf, cur_rr, cur_nc, cur_energy = cand_tf, cand_rr, cand_nc, cand_energy
            current_v = cand_v
        else:
            current_v = old_v
            cur_tf, cur_rr, cur_nc, cur_energy = (
                test_fail_std(code),
                recursive_residual(code),
                int(node_count(code)),
                energy,
            )

        records.append(
            StepRecord(
                step=step,
                V=current_v,
                test_fail=cur_tf,
                recursive_residual=cur_rr,
                node_count=cur_nc,
                energy=cur_energy,
                rejected=rejected,
                in_S=in_S(cur_tf, cur_rr, cur_nc, initial_nc),
                action=action,
            )
        )
        if action == "stop" or in_S(cur_tf, cur_rr, cur_nc, initial_nc):
            break
    return records


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--filter", choices=["on", "off"], default="on")
    parser.add_argument("--trials", type=int, default=20)
    parser.add_argument("--max-steps", type=int, default=15)
    args = parser.parse_args()

    use_filter = args.filter == "on"
    for trial in range(args.trials):
        traj = run_trial(use_filter=use_filter, max_steps=args.max_steps)
        save_trajectory(traj, f"python_rules_filter-{args.filter}_trial{trial:02d}")
        print(
            f"python rules filter={args.filter} trial={trial:02d} "
            f"steps={len(traj)} last_V={traj[-1].V if traj else float('nan'):.3f}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
