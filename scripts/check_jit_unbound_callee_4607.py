#!/usr/bin/env python3
"""Issue #4607 source-cite gate: --jit did not report an unbound variable.

Root cause: the lowering resolves a variable reference against the scope
chain (local / captured / top-level define cell), the free-var map, the
top-level value cells, the define cache, the primitive table and the self
name. A name that resolved to NOTHING was compiled as the constant 0 --
the historical "unknown reference" encoding. The eval face never compiles
such a reference: needs_tree_walker_fallback (service.ixx) routes every
program with an unresolved name to the tree walker, which raises the
canonical "unbound variable: <name>" diagnostic (evaluator_eval_flat.cpp,
Variable case). The --jit path had no such check, so the native dispatch
treated the constant 0 as closure id 0: a live closure (wrong callee ->
infinite recursion -> stack overflow -> SIGSEGV) or an out-of-range id
(the bounds-fail echo returned the 0 silently).

Fix: the lowering records every name it could not resolve (the
unbound-global sink) and exec_jit refuses to compile such a program: it
takes the tree walker (the eval face) instead, which raises the canonical
error with eval's message, eval's source location and eval's non-zero
exit -- and, for a reference that is never reached, eval's result (no
error). The refuse happens before anything is compiled or executed, so no
program state is produced twice.

ACs:
  AC1  src/compiler/lowering_impl.cpp: the Variable case records the
       unresolved name into the sink (unbound_globals->emplace_back) and
       cites #4607; the substitution to the constant 0 is the documented
       "unknown reference" encoding.
  AC2  src/compiler/lowering.ixx: the LoweringState sink field and the
       lower_to_ir / lower_to_ir_result sink parameter exist.
  AC3  src/compiler/service.ixx: exec_jit refuses to compile a program
       with an unresolved name (the sink check sits in exec_jit, BEFORE
       the compile stage) and takes the tree walker -- the eval face that
       owns the canonical diagnostic.
  AC4  tests/compiler/test_jit_unbound_callee.cpp: the runtime door
       -- the sink in TAIL / NON-TAIL / HIGHER-ORDER callee position, the
       no-false-positive controls, the source-cite rows.
  AC5  tests/python/test_regression.py: the behavioural rows -- the issue's
       rows plus the tail / non-tail / higher-order positions, each
       compared against eval in the eval / ir / jit modes.
  AC6  Wiring: build.py registers this linter and
       scripts/coverage/root_check_allowlist.txt lists it.
  AC7  No docs/design/4607-* markdown (#1655) and no dedicated
       tests/**/test_issue_4607*.cpp file (#81934).

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOW = ROOT / "src" / "compiler" / "lowering_impl.cpp"
LOW_IXX = ROOT / "src" / "compiler" / "lowering.ixx"
SVC = ROOT / "src" / "compiler" / "service.ixx"
WALK = ROOT / "src" / "compiler" / "evaluator_eval_flat.cpp"
TST = ROOT / "tests" / "compiler" / "test_jit_unbound_callee.cpp"
PY = ROOT / "tests" / "python" / "test_regression.py"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

failures: list[str] = []


def must(cond: bool, row: str) -> None:
    if not cond:
        failures.append(row)


def window(src: str, start: str, size: int) -> tuple[int, str]:
    """Locate `start` and return (index, window) with -1/"" when absent."""
    at = src.find(start)
    if at < 0:
        return -1, ""
    return at, src[at : at + size]


def main() -> int:
    low = LOW.read_text() if LOW.exists() else ""
    low_ixx = LOW_IXX.read_text() if LOW_IXX.exists() else ""
    svc = SVC.read_text() if SVC.exists() else ""
    walk = WALK.read_text() if WALK.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    py = PY.read_text() if PY.exists() else ""
    build = BUILD.read_text() if BUILD.exists() else ""
    allow = ALLOW.read_text() if ALLOW.exists() else ""

    # ── AC1: the lowering records the unresolved name ──────────────
    must("Issue #4607" in low, "AC1: lowering TU cites #4607")
    at, body = window(low, "case NodeTag::Variable: {", 20000)
    must(at >= 0, "AC1: the lowering Variable case present")
    if at >= 0:
        rec = body.find("unbound_globals->emplace_back")
        must(rec >= 0, "AC1: the Variable case records the unresolved name")
        must("Issue #4607" in body, "AC1: the Variable case documents #4607")
        must("unbound variable" in body, "AC1: the canonical face is cited")
        sub = body.find("IROpcode::ConstI64")
        must(
            sub >= 0 and rec >= 0 and rec < sub,
            "AC1: the unresolved name is recorded before the constant-0 substitution",
        )

    # ── AC2: the sink plumbing ────────────────────────────────────
    must("unbound_globals" in low_ixx, "AC2: the lowering module exposes the sink")
    must("LoweringState" in low_ixx, "AC2: the lowering module still declares LoweringState")
    must(
        low_ixx.count("unbound_globals") >= 2,
        "AC2: the sink is a LoweringState field and a lower_to_ir parameter",
    )

    # ── AC3: exec_jit refuses to compile and takes the walker ─────
    must("Issue #4607" in svc, "AC3: driver TU cites #4607")
    at, body = window(svc, "[[nodiscard]] EvalResult exec_jit(", 40000)
    must(at >= 0, "AC3: exec_jit entry present")
    if at >= 0:
        refuse = body.find("if (!unbound_globals.empty())")
        must(refuse >= 0, "AC3: exec_jit refuses a program with an unresolved name")
        must("evaluator_.eval_flat(" in body, "AC3: the refuse takes the tree walker")
        must("macro_expand_all" in body, "AC3: the refuse expands macros like eval/eval_ir")
        compile_at = body.find("jit_.compile")
        must(
            refuse >= 0 and compile_at >= 0 and refuse < compile_at,
            "AC3: the refuse precedes the compile stage (no code is compiled)",
        )
    must("needs_tree_walker_fallback" in svc, "AC3: the eval-face gate is cited")
    must("ErrorKind::UnboundVariable" in walk, "AC3: the canonical unbound-variable face lives in the tree walker")

    # ── AC4: the runtime door ─────────────────────────────────────
    must(TST.exists(), "AC4: the runtime door test file exists")
    must("ac4607" in tst or "AC1" in tst, "AC4: the door states its ACs")
    must("lower_unbound" in tst, "AC4: the door lowers a program and reads the sink")
    must('has_name(tail, "nope")' in tst, "AC4: tail-position row")
    must('has_name(non_tail, "nope")' in tst, "AC4: non-tail-position row")
    must('has_name(higher, "nope")' in tst, "AC4: higher-order-position row")
    must('has_name(form, "define-syntax")' in tst, "AC4: the issue's unbound-form row")
    must("prim.empty()" in tst, "AC4: the no-false-positive control (primitive)")
    must("param.empty()" in tst, "AC4: the no-false-positive control (parameter)")
    must("define.empty()" in tst, "AC4: the no-false-positive control (top-level define)")

    # ── AC5: the behavioural rows ──────────────────────────────────
    must("test_jit_unbound_callee_4607" in py, "AC5: the behavioural test exists")
    must("unbound-callee-tail-4607" in py, "AC5: tail-position row")
    must("unbound-callee-non-tail-4607" in py, "AC5: non-tail-position row")
    must("unbound-callee-higher-order-4607" in py, "AC5: higher-order-position row")
    must("unbound variable: " in py, "AC5: the canonical message is asserted")
    must("never-reached" in py, "AC5: the never-reached parity row is asserted")

    # ── AC6: wiring ────────────────────────────────────────────────
    must("check_jit_unbound_callee_4607.py" in build, "AC6: build.py registers this linter")
    must("check_jit_unbound_callee_4607.py" in allow, "AC6: root_check_allowlist.txt lists this linter")

    # ── AC7: house rules ───────────────────────────────────────────
    must(not list(ROOT.glob("docs/design/4607-*")), "AC7: no docs/design/4607 markdown (#1655)")
    must(not list(ROOT.glob("tests/**/test_issue_4607*.cpp")), "AC7: no dedicated test_issue_4607 file (#81934)")

    if failures:
        print(f"\n{len(failures)} row(s) failed")
        for f in failures:
            print(f"  FAIL: {f}")
        return 1
    print("\nall rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
