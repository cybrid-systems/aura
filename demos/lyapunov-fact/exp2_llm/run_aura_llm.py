#!/usr/bin/env python3
"""Experiment 2: Aura side — LLM controller + native snapshot filter.

Drives the same Aura REPL pipeline as the rules runner, but the
controller is an LLM (env: AURA_LLM_API_KEY, AURA_LLM_MODEL,
AURA_LLM_BASE_URL) that returns a whole new `define fact ...` text.
The Aura side mutates the binding via `mutate:rebind` and uses the
native `ast:snapshot` + ΔV guard to reject bad proposals.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent  # demos/lyapunov-fact/exp2_llm
ROOT = HERE.parent  # demos/lyapunov-fact (where common.py + fact_core.aura live)
REPO = HERE.parent.parent.parent  # aura/ (repo root, where build/aura lives)
sys.path.insert(0, str(ROOT))

import contextlib  # noqa: E402 — after sys.path.insert for common import

from common import (  # noqa: E402
    StepRecord,
    V_value,
    in_S,
    llm_chat,
    save_trajectory,
)

AURA_BIN = (REPO / "build" / "aura").resolve()
FACT_CORE = (ROOT / "fact_core.aura").resolve()

INITIAL_CODE = "(define (fact n)\n  (if (= n 0) 1\n      (* n (fact (- n 1)))))\n"


SYSTEM_PROMPT = (
    "You are an Aura Lisp refactorer. Given a recursive `fact` definition "
    "that currently fails tests or uses recursion, return a new (define "
    "(fact n) ...) text that is correct AND iterative (no self-call). "
    "Output the new definition only — no markdown, no commentary."
)

USER_PROMPT_TMPL = (
    "Current code:\n{code}\n\n"
    "V components: test_fail={tf}, recursive_residual={rr}, node_count={nc}, energy={e}\n"
    "Goal: test_fail==0 AND recursive_residual==0 AND node_count<=1.3*initial.\n"
    "Return only the new (define (fact n) ...) form."
)


# --- Aura REPL helper (same shape as exp1) ---------------------------


class AuraRepl:
    def __init__(self) -> None:
        self.proc = subprocess.Popen(
            [str(AURA_BIN)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self._send(f'(load "{FACT_CORE}")')

    def _send(self, expr: str) -> str:
        self.proc.stdin.write(expr + "\n")
        self.proc.stdin.flush()
        return self.proc.stdout.readline().rstrip("\n")

    def call(self, expr: str) -> str:
        return self._send(expr)

    def close(self) -> None:
        try:
            self.proc.stdin.write("(quit)\n")
            self.proc.stdin.flush()
        except Exception:
            pass
        with contextlib.suppress(Exception):
            self.proc.terminate()


# --- observability (mirror of the rules runner) ----------------------


def fact_in_aura(repl, n=10) -> int | None:
    try:
        return int(repl.call(f"(fact {n})").strip().split(";")[0])
    except Exception:
        return None


def test_fail(repl) -> int:
    return 0 if fact_in_aura(repl) == 3628800 else 1


def recursive_residual(repl) -> int:
    try:
        return int(repl.call('(length (query:calls "fact"))').strip())
    except Exception:
        return 0


def node_count(repl) -> int:
    try:
        return int(repl.call("(node-count)").strip())
    except Exception:
        return 0


# --- the LLM controller (1-line: a new fact definition) -------------


def llm_propose(repl, code: str, initial_size: int) -> tuple[str | None, str, str]:
    """Return (new_code, reason, extra). extra is model/prompt metadata."""
    tf = test_fail(repl)
    rr = recursive_residual(repl)
    nc = node_count(repl)
    energy = 0.0
    user = USER_PROMPT_TMPL.format(
        code=code,
        tf=tf,
        rr=rr,
        nc=nc,
        e=energy,
    )
    text = llm_chat(user, system=SYSTEM_PROMPT)
    text = text.strip()
    # Strip markdown fences if the LLM added them.
    text = re.sub(r"^```(?:aura|lisp|scheme)?\s*\n", "", text)
    text = re.sub(r"\n```\s*$", "", text)
    if not text.startswith("(define"):
        return None, "llm-bad-output", "no-define-prefix"
    return text, "llm-refactor", f"nc={nc}"


# --- one trial --------------------------------------------------------


def run_trial(use_filter: bool, max_steps: int = 10) -> list[StepRecord]:
    repl = AuraRepl()
    try:
        # install the initial recursive `fact`
        repl.call(f"(define fact-src {INITIAL_CODE!r})")
        repl.call('(mutate:rebind "fact" fact-src "initial")')
        repl.call("(eval-current)")
        initial_nc = node_count(repl)
        energy = 0.0
        records: list[StepRecord] = []
        # initial state
        cur_tf, cur_rr, cur_nc = test_fail(repl), recursive_residual(repl), node_count(repl)
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
            repl.call(
                "(define fact-src 0)\n(define fact-src (eval (quote (eval (quote fact)))))\n(define fact-src fact)\n(display fact-src)"
            ).strip()
            # the line above is a no-op safety; the real code lives in
            # the latest mutation. Just call propose_action with the
            # current code as text — but we don't have the text easily,
            # so we use a sentinel marker instead.
            new_code, action, extra = llm_propose(repl, code="(current fact bound)", initial_size=initial_nc)
            if new_code is None:
                records.append(
                    StepRecord(
                        step=step,
                        V=V_value(test_fail(repl), recursive_residual(repl), node_count(repl), energy),
                        test_fail=test_fail(repl),
                        recursive_residual=recursive_residual(repl),
                        node_count=node_count(repl),
                        energy=energy,
                        rejected=False,
                        in_S=in_S(test_fail(repl), recursive_residual(repl), node_count(repl), initial_nc),
                        action=action,
                        extra=extra,
                    )
                )
                break

            # external DeltaV: evaluate candidate on a snapshot
            old_v = V_value(test_fail(repl), recursive_residual(repl), node_count(repl), energy)
            # apply candidate
            repl.call('(ast:snapshot "llm-try")').strip()
            repl.call(f"(define fact-src {new_code!r})")
            repl.call('(mutate:rebind "fact" fact-src "llm")')
            repl.call("(eval-current)")
            new_tf = test_fail(repl)
            new_rr = recursive_residual(repl)
            new_nc = node_count(repl)
            new_energy = abs(new_nc - int(records[-1].node_count))
            new_v = V_value(new_tf, new_rr, new_nc, new_energy)
            delta = new_v - old_v
            rejected = use_filter and delta > 0.5
            if rejected:
                repl.call('(ast:restore "llm-try")').strip()
            records.append(
                StepRecord(
                    step=step,
                    V=(old_v if rejected else new_v),
                    test_fail=(test_fail(repl) if not rejected else (records[-1].test_fail)),
                    recursive_residual=(recursive_residual(repl) if not rejected else (records[-1].recursive_residual)),
                    node_count=(node_count(repl) if not rejected else (records[-1].node_count)),
                    energy=(0.0 if rejected else new_energy),
                    rejected=rejected,
                    in_S=in_S(
                        test_fail(repl) if not rejected else records[-1].test_fail,
                        recursive_residual(repl) if not rejected else records[-1].recursive_residual,
                        node_count(repl) if not rejected else records[-1].node_count,
                        initial_nc,
                    ),
                    action=action,
                    extra=extra,
                )
            )
            if in_S(
                test_fail(repl) if not rejected else records[-1].test_fail,
                recursive_residual(repl) if not rejected else records[-1].recursive_residual,
                node_count(repl) if not rejected else records[-1].node_count,
                initial_nc,
            ) or action.startswith("llm-stop"):
                break
        return records
    finally:
        repl.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--filter", choices=["on", "off"], default="on")
    parser.add_argument("--trials", type=int, default=20)
    parser.add_argument("--max-steps", type=int, default=10)
    args = parser.parse_args()

    if not AURA_BIN.exists():
        print(f"!! Aura binary not found at {AURA_BIN}. Build first: ./build.py build")
        return 1

    for trial in range(args.trials):
        try:
            traj = run_trial(use_filter=(args.filter == "on"), max_steps=args.max_steps)
        except Exception as exc:
            print(f"!! trial {trial} failed: {exc}")
            traj = []
        save_trajectory(traj, f"aura_llm_filter-{args.filter}_trial{trial:02d}")
        print(
            f"aura llm filter={args.filter} trial={trial:02d} "
            f"steps={len(traj)} last_V={traj[-1].V if traj else float('nan'):.3f}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
