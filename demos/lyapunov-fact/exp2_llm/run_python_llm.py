#!/usr/bin/env python3
"""Experiment 2: Python side — LLM controller + external filter.

Mirrors `exp2_llm/run_aura_llm.py`: same env vars, same LLM, same
prompt shape, same threshold. The only difference is the execution
model: Python uses whole-function replacement + a deepcopy-based
external filter. The candidate code is eval()'d in a fresh namespace
to compute V before deciding accept/reject.
"""

from __future__ import annotations

import argparse
import ast
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(ROOT))

from common import (  # noqa: E402
    StepRecord,
    V_value,
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
    "You are a Python refactorer. Given a recursive `fact` definition that "
    "currently fails tests or uses recursion, return a new Python function "
    "`def fact(n): ...` that is correct AND iterative (no self-call). "
    "Output only the function definition — no markdown, no commentary."
)

USER_PROMPT_TMPL = (
    "Current code:\n{code}\n\n"
    "V components: test_fail={tf}, recursive_residual={rr}, node_count={nc}, energy={e}\n"
    "Goal: test_fail==0 AND recursive_residual==0 AND node_count<=1.3*initial.\n"
    "Return only the new def fact(n): ... block."
)


# --- Python-side observability (mirrors exp1) --------------------------

TEST_VALUE = 3628800


def _exec_fact(code: str, n: int = 10) -> int | None:
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
    return 0 if _exec_fact(code) == TEST_VALUE else 1


def recursive_residual(code: str) -> int:
    return code.count("fact(") - 1


def node_count(code: str) -> int:
    try:
        tree = ast.parse(code)
    except SyntaxError:
        return float("inf")
    return float(sum(1 for _ in ast.walk(tree)))


def V(code: str, energy: float) -> float:
    return V_value(test_fail(code), recursive_residual(code), node_count(code), energy)


# --- the LLM controller -----------------------------------------------


def llm_propose(code: str) -> tuple[str | None, str, str]:
    """Return (new_code, reason, extra). code is the full new def fact."""
    user = USER_PROMPT_TMPL.format(
        code=code,
        tf=test_fail(code),
        rr=recursive_residual(code),
        nc=node_count(code),
        e=0.0,
    )
    text = llm_chat(user, system=SYSTEM_PROMPT)
    text = text.strip()
    text = re.sub(r"^```(?:python)?\s*\n", "", text)
    text = re.sub(r"\n```\s*$", "", text)
    if not text.startswith("def fact"):
        return None, "llm-bad-output", "no-def-prefix"
    return text, "llm-refactor", "ok"


# --- one trial --------------------------------------------------------


def run_trial(use_filter: bool, max_steps: int = 10) -> list[StepRecord]:
    code = INITIAL_CODE
    energy = 0.0
    initial_nc = node_count(code)
    records: list[StepRecord] = []
    cur_tf, cur_rr, cur_nc = test_fail(code), recursive_residual(code), int(node_count(code))
    records.append(
        StepRecord(
            step=0,
            V=V_value(cur_tf, cur_rr, cur_nc, energy),
            test_fail=cur_tf,
            recursive_residual=cur_rr,
            node_count=cur_nc,
            energy=energy,
            rejected=False,
            in_S=in_S(cur_tf, cur_rr, cur_nc, initial_nc),
            action="init",
            extra=f"initial_nc={initial_nc}",
        )
    )
    for step in range(1, max_steps + 1):
        new_code, action, extra = llm_propose(code)
        if new_code is None:
            records.append(
                StepRecord(
                    step=step,
                    V=V(code, energy),
                    test_fail=cur_tf,
                    recursive_residual=cur_rr,
                    node_count=cur_nc,
                    energy=energy,
                    rejected=False,
                    in_S=in_S(cur_tf, cur_rr, cur_nc, initial_nc),
                    action=action,
                    extra=extra,
                )
            )
            break
        old_v = V(code, energy)
        # external filter: evaluate the candidate in a fresh namespace
        cand_tf = test_fail(new_code)
        cand_rr = recursive_residual(new_code)
        cand_nc = int(node_count(new_code))
        cand_energy = abs(cand_nc - cur_nc)
        cand_v = V_value(cand_tf, cand_rr, cand_nc, cand_energy)
        delta = cand_v - old_v
        rejected = use_filter and delta > 0.5
        if not rejected:
            code = new_code
            energy = cand_energy
            cur_tf, cur_rr, cur_nc, cur_v = cand_tf, cand_rr, cand_nc, cand_v
        records.append(
            StepRecord(
                step=step,
                V=cur_v,
                test_fail=cur_tf,
                recursive_residual=cur_rr,
                node_count=cur_nc,
                energy=energy,
                rejected=rejected,
                in_S=in_S(cur_tf, cur_rr, cur_nc, initial_nc),
                action=action,
                extra=extra,
            )
        )
        if in_S(cur_tf, cur_rr, cur_nc, initial_nc):
            break
    return records


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--filter", choices=["on", "off"], default="on")
    parser.add_argument("--trials", type=int, default=20)
    parser.add_argument("--max-steps", type=int, default=10)
    args = parser.parse_args()

    for trial in range(args.trials):
        try:
            traj = run_trial(use_filter=(args.filter == "on"), max_steps=args.max_steps)
        except Exception as exc:
            print(f"!! trial {trial} failed: {exc}")
            traj = []
        save_trajectory(traj, f"python_llm_filter-{args.filter}_trial{trial:02d}")
        print(
            f"python llm filter={args.filter} trial={trial:02d} "
            f"steps={len(traj)} last_V={traj[-1].V if traj else float('nan'):.3f}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
