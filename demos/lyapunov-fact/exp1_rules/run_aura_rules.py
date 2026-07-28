#!/usr/bin/env python3
"""Experiment 1: Aura — rule controller + native ast:snapshot filter."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT))

from aura_driver import (  # noqa: E402
    AuraRepl,
    install,
    node_count,
    recursive_residual,
    restore,
    snapshot,
    test_fail,
)
from common import (  # noqa: E402
    FILTER_DELTA_V,
    StepRecord,
    V_value,
    energy_of,
    in_S,
    save_trajectory,
)

INITIAL_CODE = "(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))"
GOOD_ITER = "(define (fact n) (let loop ((i n) (acc 1)) (if (= i 0) acc (loop (- i 1) (* acc i)))))"


def measure(repl: AuraRepl, energy: float):
    tf, rr, nc = test_fail(repl), recursive_residual(repl), node_count(repl)
    return tf, rr, nc, V_value(tf, rr, nc, energy)


def propose_action(repl: AuraRepl) -> tuple[str | None, str]:
    tf, rr, nc, _ = measure(repl, 0.0)
    if tf == 1:
        return GOOD_ITER, "fix-correctness"
    if rr > 0:
        return GOOD_ITER, "remove-recursion"
    if nc > 25:
        return GOOD_ITER, "simplify"
    return None, "stop"


def run_trial(repl: AuraRepl, use_filter: bool, max_steps: int = 15) -> list[StepRecord]:
    install(repl, INITIAL_CODE)
    energy = 0.0
    tf, rr, nc, v = measure(repl, energy)
    initial_nc = nc
    records = [
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
    ]
    for step in range(1, max_steps + 1):
        new_code, action = propose_action(repl)
        if new_code is None:
            tf, rr, nc, v = measure(repl, energy)
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

        old_v, old_nc = v, nc
        snap = snapshot(repl) if use_filter else ""
        install(repl, new_code)
        cand_energy = energy_of(old_nc, node_count(repl))
        ctf, crr, cnc, cand_v = measure(repl, cand_energy)
        delta = cand_v - old_v
        rejected = use_filter and delta > FILTER_DELTA_V
        if rejected:
            restore(repl, snap)
            tf, rr, nc, v = measure(repl, energy)
        else:
            energy = cand_energy
            tf, rr, nc, v = ctf, crr, cnc, cand_v
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
    repl = AuraRepl()
    try:
        for trial in range(args.trials):
            try:
                traj = run_trial(repl, use_filter=use_filter, max_steps=args.max_steps)
            except Exception as exc:
                print(f"!! trial {trial} failed: {exc}", flush=True)
                traj = []
            save_trajectory(traj, f"aura_rules_filter-{args.filter}_trial{trial:02d}")
            last = traj[-1] if traj else None
            print(
                f"aura rules filter={args.filter} trial={trial:02d} "
                f"steps={len(traj)} last_V={last.V if last else float('nan'):.3f} "
                f"in_S={last.in_S if last else False}",
                flush=True,
            )
    finally:
        repl.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
