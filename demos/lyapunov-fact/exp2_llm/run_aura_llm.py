#!/usr/bin/env python3
"""Experiment 2: Aura — LLM controller + native ast:snapshot filter."""

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
    llm_chat,
    save_trajectory,
)

INITIAL_CODE = "(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))"

SYSTEM_PROMPT = (
    "You are an Aura Lisp refactorer. Given a recursive (define (fact n) ...) "
    "that fails tests or uses recursion, return a correct iterative definition "
    "only — no markdown, no commentary."
)

USER_TMPL = (
    "Current code:\n{code}\n\n"
    "V components: test_fail={tf}, recursive_residual={rr}, node_count={nc}, energy={e}\n"
    "Goal: test_fail==0 AND recursive_residual==0 AND node_count<=1.3*initial.\n"
    "Return only the new (define (fact n) ...) form."
)


def measure(repl: AuraRepl, energy: float):
    tf, rr, nc = test_fail(repl), recursive_residual(repl), node_count(repl)
    return tf, rr, nc, V_value(tf, rr, nc, energy)


def llm_propose(code: str, tf: int, rr: int, nc: float) -> tuple[str | None, str, str]:
    text = llm_chat(
        USER_TMPL.format(code=code, tf=tf, rr=rr, nc=nc, e=0.0),
        system=SYSTEM_PROMPT,
    )
    if "(define" not in text:
        return None, "llm-bad-output", "no-define"
    idx = text.find("(define")
    text = text[idx:].strip()
    # keep first balanced s-expression
    depth = 0
    end = None
    for i, c in enumerate(text):
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                end = i + 1
                break
    if end is None:
        return None, "llm-bad-output", "unbalanced"
    text = text[:end]
    if "fact" not in text:
        return None, "llm-bad-output", "no-fact"
    return text, "llm-refactor", "ok"


def run_trial(use_filter: bool, max_steps: int = 10) -> list[StepRecord]:
    repl = AuraRepl()
    code = INITIAL_CODE
    try:
        install(repl, code)
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
            new_code, action, extra = llm_propose(code, tf, rr, nc)
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
                        extra=extra,
                    )
                )
                break
            old_v, old_nc = v, nc
            snap = snapshot(repl) if use_filter else ""
            try:
                install(repl, new_code)
            except Exception as exc:
                records.append(
                    StepRecord(
                        step=step,
                        V=old_v,
                        test_fail=tf,
                        recursive_residual=rr,
                        node_count=nc,
                        energy=energy,
                        rejected=True,
                        in_S=in_S(tf, rr, nc, initial_nc),
                        action="llm-install-fail",
                        extra=str(exc)[:80],
                    )
                )
                if use_filter and snap:
                    restore(repl, snap)
                continue
            cand_energy = energy_of(old_nc, node_count(repl))
            ctf, crr, cnc, cand_v = measure(repl, cand_energy)
            delta = cand_v - old_v
            rejected = use_filter and delta > FILTER_DELTA_V
            if rejected:
                restore(repl, snap)
                tf, rr, nc, v = measure(repl, energy)
            else:
                code, energy = new_code, cand_energy
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
                    extra=f"{extra};delta={delta:.3f}",
                )
            )
            if in_S(tf, rr, nc, initial_nc):
                break
        return records
    finally:
        repl.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--filter", choices=["on", "off"], default="on")
    parser.add_argument("--trials", type=int, default=10)
    parser.add_argument("--max-steps", type=int, default=10)
    args = parser.parse_args()
    for trial in range(args.trials):
        try:
            traj = run_trial(use_filter=(args.filter == "on"), max_steps=args.max_steps)
        except Exception as exc:
            print(f"!! trial {trial} failed: {exc}", flush=True)
            traj = []
        save_trajectory(traj, f"aura_llm_filter-{args.filter}_trial{trial:02d}")
        last = traj[-1] if traj else None
        print(
            f"aura llm filter={args.filter} trial={trial:02d} "
            f"steps={len(traj)} last_V={last.V if last else float('nan'):.3f} "
            f"in_S={last.in_S if last else False}",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
