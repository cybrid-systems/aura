#!/usr/bin/env python3
"""Experiment 1: Aura side — minimal rule controller + native snapshot filter.

Pipes `fact_core.aura` into Aura's REPL (one subprocess for the whole
batch). Per-trial isolation comes from re-loading fact_core + reinstalling
the initial fact at the start of each trial — REPL state persists within
the subprocess but we re-seed it explicitly per trial to avoid leaking
state across trials.

Why REPL and not `--serve`?
  `--serve`'s JSON `exec` command routes through `cs.exec_with_cache`,
  which has a caching bug — it returns the previously cached eval value
  instead of the current one (e.g. `(if #f 99 100)` returns the previous
  call's `99`, `(square 5)` returns the previous `(if #t 99 100)`'s `99`).
  REPL uses `cs.eval(line)` directly and works correctly. See
  `src/compiler/evaluator_eval_flat.cpp` for the eval paths.

Why not `(load "fact_core.aura")`?
  Aura's `(load ...)` is silent — it produces no stdout — so a `readline()`
  call would block forever waiting for a response. We sidestep this by
  parsing fact_core.aura into single-line top-level forms and sending each
  one as its own REPL line.
"""

from __future__ import annotations

import argparse
import re
import select
import subprocess
import sys
import time
from pathlib import Path

# Allow `python3 exp1_rules/run_aura_rules.py` from anywhere.
HERE = Path(__file__).resolve().parent  # demos/lyapunov-fact/exp1_rules
ROOT = HERE.parent  # demos/lyapunov-fact
REPO = HERE.parent.parent.parent  # aura/ (repo root)
sys.path.insert(0, str(ROOT))

import contextlib  # noqa: E402 — after sys.path.insert for common import

from common import StepRecord, save_trajectory  # noqa: E402

AURA_BIN = (REPO / "build" / "aura").resolve()
FACT_CORE = (ROOT / "fact_core.aura").resolve()


# --- REPL driver -------------------------------------------------------


class AuraRepl:
    """One Aura REPL subprocess for the whole batch.

    Wire protocol (REPL mode, no `--serve`):
      * stdin  → one s-expression per line (with trailing `\\n`).
      * stdout → one response line per eval: the printed value, then `\\n`.
        Aura also prints `[#NNNN ...]` warnings to stderr (we leave stderr
        alone; it doesn't pollute stdout).
    """

    def __init__(self, fact_core_path: Path) -> None:
        if not AURA_BIN.exists():
            raise FileNotFoundError(f"Aura binary not found at {AURA_BIN}. Build first: ./build.py build")
        self.proc = subprocess.Popen(
            [str(AURA_BIN)],  # REPL mode (no --serve)
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,  # line-buffered
        )
        self._fact_core_forms: list[str] = self._read_fact_core_forms(fact_core_path)

    @staticmethod
    def _read_fact_core_forms(path: Path) -> list[str]:
        """Read fact_core.aura, strip ;; line comments, return each
        top-level form as a single-line string (no embedded `\\n`).

        REPL reads one line at a time via `std::getline` and evaluates
        each one independently. Sending a multi-line form in a single
        write gets it split, the partial forms error, and the leftover
        responses desynchronise the request/response stream.
        """
        raw = path.read_text()
        cleaned: list[str] = []
        for line in raw.splitlines():
            s = line.lstrip()
            if not s or s.startswith(";;"):
                continue
            cleaned.append(line)
        body = "\n".join(cleaned)

        forms: list[str] = []
        depth = 0
        in_string = False
        in_line_comment = False
        buf: list[str] = []
        i = 0
        while i < len(body):
            c = body[i]
            if in_line_comment:
                buf.append(c)
                if c == "\n":
                    in_line_comment = False
                i += 1
                continue
            if in_string:
                buf.append(c)
                if c == "\\" and i + 1 < len(body):
                    buf.append(body[i + 1])
                    i += 2
                    continue
                if c == '"':
                    in_string = False
                i += 1
                continue
            if c == ";" and i + 1 < len(body) and body[i + 1] == ";":
                in_line_comment = True
                buf.append(c)
                i += 1
                continue
            if c == '"':
                in_string = True
                buf.append(c)
                i += 1
                continue
            if c == "(":
                depth += 1
                buf.append(c)
                i += 1
                continue
            if c == ")":
                depth -= 1
                buf.append(c)
                if depth == 0:
                    forms.append("".join(buf).strip())
                    buf = []
                i += 1
                continue
            buf.append(c)
            i += 1
        if buf or depth != 0:
            raise RuntimeError(f"fact_core.aura parse imbalance: depth={depth} leftover={''.join(buf)[:80]!r}")
        # Normalize each form to a single line so REPL gets one form per
        # write.
        normalized: list[str] = []
        for f in forms:
            if not f:
                continue
            flat = " ".join(f.split())
            if flat:
                normalized.append(flat)
        return normalized

    # --- low-level transport ------------------------------------------

    def _send(self, expr: str, timeout_s: float = 30.0) -> str:
        """Send `expr + '\\n'`, read one response line, return it stripped."""
        assert self.proc.stdin is not None and self.proc.stdout is not None
        self.proc.stdin.write(expr + "\n")
        self.proc.stdin.flush()
        deadline = time.monotonic() + timeout_s
        while True:
            remaining = max(0.01, deadline - time.monotonic())
            r, _, _ = select.select([self.proc.stdout], [], [], remaining)
            if not r:
                raise TimeoutError(f"Aura REPL did not respond within {timeout_s}s for: {expr[:120]!r}")
            raw = self.proc.stdout.readline()
            if not raw:
                raise EOFError("Aura REPL closed stdout")
            return raw.rstrip("\n")

    def eval(self, sexp: str, timeout_s: float = 30.0) -> str:
        """Eval a single s-expression in the REPL, return the printed value.

        Raises RuntimeError if Aura returns something that doesn't look
        like a value (e.g. an `error: ...` line).
        """
        resp = self._send(sexp, timeout_s=timeout_s)
        # Aura prints `error: <msg>` for parse / unbound / runtime errors.
        if resp.startswith("error:"):
            raise RuntimeError(f"Aura eval error: sexp={sexp[:120]!r} resp={resp!r}")
        return resp

    def load_fact_core(self) -> None:
        """Eval each top-level form of fact_core.aura into the REPL session."""
        for form in self._fact_core_forms:
            self.eval(form)

    # --- lifecycle -----------------------------------------------------

    def close(self) -> None:
        if self.proc.poll() is not None:
            return
        try:
            self.proc.stdin.write("\n")  # graceful EOF
            self.proc.stdin.flush()
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=2)
        except Exception:
            try:
                self.proc.terminate()
                self.proc.wait(timeout=2)
            except Exception:
                with contextlib.suppress(Exception):
                    self.proc.kill()


# --- fact_core.aura schema bridge --------------------------------------

SCHEMA = {
    "initial_code_marker": "INITIAL-CODE",
    "good_iter_marker": "GOOD-ITER",
    "step": "step",
    "node_count": "node-count",
    "recursive": "recursive-residual",
    "test_fail": "test-fail",
    "compute_v": "compute-V",
    "propose": "propose-action",
}

# Use the fact_core.aura bindings directly — avoids Python string-embed
# of multi-line source (which injects literal backslash-n sequences into
# Aura's parser and trips parse-errors).
INITIAL_CODE_SYM = "INITIAL-CODE"
GOOD_ITER_SYM = "GOOD-ITER"


# --- per-trial V computation (parallels the Python side exactly) -------

TEST_VALUE = 3628800  # 10!


def fact_in_aura(repl: AuraRepl, n: int) -> int | None:
    raw = repl.eval(f"(fact {n})").strip()
    try:
        return int(raw.split(";")[0].strip())
    except Exception:
        return None


def test_fail(repl: AuraRepl) -> int:
    return 0 if fact_in_aura(repl, 10) == TEST_VALUE else 1


def recursive_residual(repl: AuraRepl) -> int:
    raw = repl.eval('(length (query:calls "fact"))').strip()
    try:
        return int(raw)
    except Exception:
        return 0


def node_count(repl: AuraRepl) -> int:
    raw = repl.eval("(node-count)").strip()
    try:
        return int(raw)
    except Exception:
        return 0


def V(repl: AuraRepl, energy: float) -> float:
    """Client-side V computation matches the spec exactly."""
    tf = test_fail(repl)
    rr = recursive_residual(repl)
    nc = node_count(repl)
    return 10.0 * tf + 2.0 * rr + 0.1 * nc + energy


def in_S(repl: AuraRepl, initial_size: float) -> bool:
    return test_fail(repl) == 0 and recursive_residual(repl) == 0 and node_count(repl) <= 1.3 * initial_size


# --- controller --------------------------------------------------------


def install_initial(repl: AuraRepl) -> None:
    """Bind fact-src to INITIAL-CODE → mutate:rebind → eval-current.

    `INITIAL-CODE` is already a quoted Scheme form installed by
    `load_fact_core()`, so we just point `fact-src` at it.
    """
    repl.eval(f"(define fact-src {INITIAL_CODE_SYM})")
    repl.eval('(mutate:rebind "fact" fact-src "initial")')
    repl.eval("(eval-current)")


def propose_action(repl: AuraRepl) -> tuple[str | None, str]:
    """Return (new_code, reason). Mirrors the Python controller exactly."""
    if test_fail(repl) == 1:
        return GOOD_ITER_SYM, "fix-correctness"
    if recursive_residual(repl) > 0:
        return GOOD_ITER_SYM, "remove-recursion"
    if node_count(repl) > 25:
        return GOOD_ITER_SYM, "simplify"
    return None, "stop"


# --- one trial ---------------------------------------------------------

_FLOAT_RE = re.compile(r"-?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?")
_BOOL_RE = re.compile(r"#t|#f")


def _parse_step_result(raw: str) -> tuple[bool, float, bool, str, float]:
    """Aura prints a list like (V-after); depending on the version it may
    show quoted symbols. We accept a permissive parse."""
    raw = raw.strip()
    bools = _BOOL_RE.findall(raw)
    floats = _FLOAT_RE.findall(raw)
    accepted = True
    rejected = False
    try:
        new_v = float(floats[3]) if len(floats) >= 4 else 0.0
        if len(bools) >= 2:
            rejected = bools[1] == "#t"
            accepted = not rejected
        new_energy = float(floats[4]) if len(floats) >= 5 else 0.0
    except Exception:
        new_v, rejected, new_energy = 0.0, False, 0.0
        accepted = not rejected
    return accepted, new_v, rejected, "step", new_energy


def run_trial(repl: AuraRepl, trial_idx: int, use_filter: bool, max_steps: int = 15) -> list[StepRecord]:
    """Run one trial in a logically-fresh REPL state.

    Per-trial isolation strategy (REPL state persists in the subprocess
    but we re-seed at the start of each trial):
      1. Re-load fact_core.aura forms → reinstalls INITIAL-CODE, GOOD-ITER,
         node-count, recursive-residual, test-fail, compute-V, propose-action,
         step.
      2. install_initial → binds `fact-src` to INITIAL-CODE + mutate:rebind.
      3. Run controller steps until S reached or max_steps.
    """
    # Re-seed definitions for a clean state per trial.
    repl.load_fact_core()
    install_initial(repl)
    repl.eval("(eval-current)")

    initial_nc = node_count(repl)
    energy = 0.0
    records: list[StepRecord] = []
    old_v = V(repl, energy)
    records.append(
        StepRecord(
            step=0,
            V=old_v,
            test_fail=test_fail(repl),
            recursive_residual=recursive_residual(repl),
            node_count=initial_nc,
            energy=energy,
            rejected=False,
            in_S=in_S(repl, initial_nc),
            action="init",
        )
    )
    for step in range(1, max_steps + 1):
        new_code, action = propose_action(repl)
        if new_code is None:
            records.append(
                StepRecord(
                    step=step,
                    V=V(repl, energy),
                    test_fail=test_fail(repl),
                    recursive_residual=recursive_residual(repl),
                    node_count=node_count(repl),
                    energy=energy,
                    rejected=False,
                    in_S=in_S(repl, initial_nc),
                    action=action,
                )
            )
            break

        # Aura (step use-filter? energy) returns a 5-tuple.
        raw = repl.eval(f"(step {'#t' if use_filter else '#f'} {energy})").strip()
        accepted, new_v, rejected, reason, new_energy = _parse_step_result(raw)
        # Redo the rebind explicitly so subsequent steps see the new code.
        repl.eval(f"(define fact-src {new_code})")
        repl.eval('(mutate:rebind "fact" fact-src "rebind")')
        repl.eval("(eval-current)")
        energy = 0.0 if rejected else new_energy
        records.append(
            StepRecord(
                step=step,
                V=new_v,
                test_fail=test_fail(repl),
                recursive_residual=recursive_residual(repl),
                node_count=node_count(repl),
                energy=energy,
                rejected=rejected,
                in_S=in_S(repl, initial_nc),
                action=reason,
            )
        )
        if action == "stop" or in_S(repl, initial_nc):
            break
    return records


# --- main --------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--filter", choices=["on", "off"], default="on")
    parser.add_argument("--trials", type=int, default=20)
    parser.add_argument("--max-steps", type=int, default=15)
    args = parser.parse_args()

    use_filter = args.filter == "on"
    repl = AuraRepl(FACT_CORE)
    try:
        for trial in range(args.trials):
            try:
                traj = run_trial(repl, trial, use_filter=use_filter, max_steps=args.max_steps)
            except Exception as exc:  # pragma: no cover - defensive
                print(f"!! trial {trial} failed: {exc}", flush=True)
                traj = []
            save_trajectory(traj, f"aura_rules_filter-{args.filter}_trial{trial:02d}")
            print(
                f"aura rules filter={args.filter} trial={trial:02d} "
                f"steps={len(traj)} last_V={traj[-1].V if traj else float('nan'):.3f}",
                flush=True,
            )
    finally:
        repl.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
