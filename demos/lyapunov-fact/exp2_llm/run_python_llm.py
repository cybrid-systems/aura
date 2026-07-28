#!/usr/bin/env python3
"""Experiment 2: Python — LLM controller + external ΔV filter."""

from __future__ import annotations

import argparse
import ast
import re
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
    llm_chat,
    save_trajectory,
)

INITIAL_CODE = """\
def fact(n):
    if n == 0:
        return 1
    return n * fact(n - 1)
"""

SYSTEM_PROMPT = (
    "You are a Python refactorer. Given a recursive `fact` that fails tests "
    "or uses recursion, return a correct iterative `def fact(n): ...` only — "
    "no markdown, no commentary."
)

USER_TMPL = (
    "Current code:\n{code}\n\n"
    "V components: test_fail={tf}, recursive_residual={rr}, node_count={nc}, energy={e}\n"
    "Goal: test_fail==0 AND recursive_residual==0 AND node_count<=1.3*initial.\n"
    "Return only the new def fact(n): ... block."
)


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
    return max(0, code.count("fact(") - 1)


def node_count(code: str) -> float:
    try:
        return float(sum(1 for _ in ast.walk(ast.parse(code))))
    except SyntaxError:
        return float("inf")


def measure(code: str, energy: float):
    tf, rr, nc = test_fail(code), recursive_residual(code), node_count(code)
    return tf, rr, nc, V_value(tf, rr, nc, energy)


def llm_propose(code: str) -> tuple[str | None, str, str]:
    tf, rr, nc, _ = measure(code, 0.0)
    text = llm_chat(
        USER_TMPL.format(code=code, tf=tf, rr=rr, nc=nc, e=0.0),
        system=SYSTEM_PROMPT,
    )
    if "def fact" not in text:
        return None, "llm-bad-output", "no-def"
    idx = text.find("def fact")
    text = text[idx:].strip()
    # keep only the first top-level def (drop trailing chatter)
    m = re.search(r"(?ms)^def fact\b.*?(?=^def |\Z)", text)
    if m:
        text = m.group(0).strip()
    try:
        ast.parse(text)
    except SyntaxError as exc:
        return None, "llm-bad-syntax", str(exc)[:60]
    return text, "llm-refactor", "ok"


def run_trial(use_filter: bool, max_steps: int = 10) -> list[StepRecord]:
    code = INITIAL_CODE
    energy = 0.0
    tf, rr, nc, v = measure(code, energy)
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
        new_code, action, extra = llm_propose(code)
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
                    extra=extra,
                )
            )
            break
        old_v = v
        new_nc = node_count(new_code)
        if new_nc == float("inf"):
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
                    action="llm-invalid-ast",
                    extra=extra,
                )
            )
            continue
        cand_energy = energy_of(nc, new_nc)
        ctf, crr, cnc, cand_v = measure(new_code, cand_energy)
        delta = cand_v - old_v
        rejected = use_filter and delta > FILTER_DELTA_V
        if not rejected:
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
        save_trajectory(traj, f"python_llm_filter-{args.filter}_trial{trial:02d}")
        last = traj[-1] if traj else None
        print(
            f"python llm filter={args.filter} trial={trial:02d} "
            f"steps={len(traj)} last_V={last.V if last else float('nan'):.3f} "
            f"in_S={last.in_S if last else False}",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
