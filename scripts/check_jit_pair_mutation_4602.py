#!/usr/bin/env python3
"""Issue #4602 source-cite gate: a pair mutation must be visible to the
next car/cdr in every execution mode.

The issue: under --jit, (set-car! p 9) on a pair the frame allocated left
the pair's car at its OLD value, so (car p) answered 1 instead of 9.
--ir / eval answered 9.

Mechanism (the issue's own note pointed at it): set-car! is not a JIT
opcode and not a prim_call_map entry -- it reaches the evaluator through
the GENERAL CALL. A JIT'd OpCall lowers to aura_closure_call ->
aura_closure_dispatch_native_checked, and an evaluator-level primitive
loaded from a global/define cell is a RefPrimitive-tagged word
((slot << kRefPoolShift) | (RefPrimitive << kRefTypeShift) | kRefLowTag).
The dispatcher's bounds-fail fallthrough echoed that raw word back to the
JIT'd caller ("return callee_val for non-callable"), so the evaluator
primitive NEVER RAN: there was no write at all, and the JIT's car read
the untouched pair. #4606 answers a RefPrimitive callee word with the
leave-native refuse (#4084 sentinel) and exec_jit re-runs the restored
IR in the interpreter, where the interpreter's is_primitive OpCall arm
executes the identical call -- the write lands and the result is
eval-identical.

So this gate pins the mechanism the fix depends on, the write side's
tenant-stamp contract, and the REGRESSION GUARD that keeps the symptom
from coming back:

  AC1  src/compiler/aura_jit_runtime.cpp: the blessed dispatch entry
       (aura_closure_dispatch_native_checked) refuses a RefPrimitive
       callee word with closure_dispatch_leave_native(), AFTER the
       workspace-read unlock and BEFORE the legacy "match IRInterpreter"
       callee_val echo. The echo is what made the set-car! write vanish,
       so the refuse must stay ahead of it.
  AC2  src/compiler/service.ixx: exec_jit reads the #4084 sentinel and
       re-runs the restored IR in the interpreter (eval_ir(input)) -- the
       driver half of the refuse. Without it the refused frame would
       surface "jit prologue deopt" instead of running the primitive.
  AC3  src/compiler/evaluator_primitives_pair.cpp: the set-car! /
       set-cdr! primitives keep the #4298 in-range owner-stamp gate and
       the #4057 process-level g_pair_slots arm (owner stamp compared
       against the caller principal under the production face, slot
       unchanged on refusal). The issue requires the tenant stamp checks
       on the write -- no arm may be dropped or reordered.
  AC4  tests/python/test_regression.py: the #4602 guard, read from the
       parsed AST (not from source text, so a reformat cannot hide a
       regression):
       (a) the four corpus rows in the 5-mode parity harness
           (ir / eval / jit / jitf1 / jitf2): pair-setcar-let-4602,
           pair-setcar-arg-4602, pair-setcdr-arg-4602 and
           pair-setcar-twice-4602, each with its expected value;
       (b) the set-car-escape row of the scalar-pairs corpus is NO
           LONGER excluded from the parity check and now expects 11
           (car 9 + cdr 2) in every mode;
       (c) the tier-differential corpus carries the pair-mutate template
           (a set-car! then a car) over every mode, including the
           baseline jit (L1/L2/SR off).
  AC5  Wiring: build.py registers this linter and
       scripts/coverage/root_check_allowlist.txt lists it.
  AC6  House rules: no tests/**/test_issue_4602*.cpp, no
       docs/design/4602-*.

Exit 0 = all rows satisfied. Exit 1 = a cited row regressed.
"""

from __future__ import annotations

import ast
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RT = ROOT / "src" / "compiler" / "aura_jit_runtime.cpp"
SVC = ROOT / "src" / "compiler" / "service.ixx"
PAIR = ROOT / "src" / "compiler" / "evaluator_primitives_pair.cpp"
REG = ROOT / "tests" / "python" / "test_regression.py"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

failures: list[str] = []


def must(cond: bool, row: str) -> None:
    if not cond:
        failures.append(row)


def window(src: str, start: str, size: int) -> tuple[int, str]:
    """Locate start and return (index, window); (-1, \"\") when absent."""
    at = src.find(start)
    if at < 0:
        return -1, ""
    return at, src[at : at + size]


def _const(node: ast.AST):
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    return None


def _func(tree: ast.Module, name: str):
    for node in ast.walk(tree):
        if isinstance(node, ast.FunctionDef) and node.name == name:
            return node
    return None


def _corpus_rows(fn) -> dict:
    """add(programs, name, program, expect=...) -> {name: (program, expect)}."""
    rows: dict = {}
    for node in ast.walk(fn):
        if not isinstance(node, ast.Call):
            continue
        if not isinstance(node.func, ast.Name) or node.func.id != "add":
            continue
        if len(node.args) < 3:
            continue
        name = _const(node.args[1])
        program = _const(node.args[2])
        if name is None or program is None:
            continue
        expect = None
        for kw in node.keywords:
            if kw.arg == "expect":
                expect = _const(kw.value)
        rows[name] = (program, expect)
    return rows


def _tuple_rows(fn) -> dict:
    """A `programs = [...]` list of string tuples -> {first: tuple}."""
    rows: dict = {}
    for node in ast.walk(fn):
        if not isinstance(node, ast.Assign):
            continue
        if not any(isinstance(t, ast.Name) and t.id == "programs" for t in node.targets):
            continue
        if not isinstance(node.value, ast.List):
            continue
        for elt in node.value.elts:
            if not isinstance(elt, ast.Tuple):
                continue
            vals = tuple(_const(e) for e in elt.elts)
            if vals and isinstance(vals[0], str):
                rows[vals[0]] = vals
    return rows


def _dict_entries(fn, var: str) -> dict:
    """A `var = {...}` dict of string->string -> {key: value}."""
    out: dict = {}
    for node in ast.walk(fn):
        if not isinstance(node, ast.Assign):
            continue
        if not any(isinstance(t, ast.Name) and t.id == var for t in node.targets):
            continue
        if not isinstance(node.value, ast.Dict):
            continue
        for key, val in zip(node.value.keys, node.value.values, strict=False):
            k, v = _const(key), _const(val)
            if k is not None and v is not None:
                out[k] = v
    return out


def _compares_against(fn, name: str, value: str) -> bool:
    """True if the function compares the loop variable `name` to `value`."""
    for node in ast.walk(fn):
        if not isinstance(node, ast.Compare):
            continue
        if not any(isinstance(op, ast.Eq) for op in node.ops):
            continue
        for side in [node.left, *node.comparators]:
            if isinstance(side, ast.Name) and side.id == name and _const(node.comparators[0]) == value:
                return True
    return False


def main() -> int:
    rt = RT.read_text() if RT.exists() else ""
    svc = SVC.read_text() if SVC.exists() else ""
    pair = PAIR.read_text() if PAIR.exists() else ""
    reg = REG.read_text() if REG.exists() else ""
    build = BUILD.read_text() if BUILD.exists() else ""
    allow = ALLOW.read_text() if ALLOW.exists() else ""

    # -- AC1: the refuse arm ahead of the legacy callee_val echo -------
    must("Issue #4606" in rt, "AC1: runtime TU cites #4606")
    at, body = window(rt, "int64_t aura_closure_dispatch_native_checked(", 3200)
    must(at >= 0, "AC1: blessed dispatch entry present")
    if at >= 0:
        arm = body.find("Issue #4606")
        must(arm > 0, "AC1: #4606 arm inside the entry body")
        must(
            "aura::compiler::types::RefPrimitive" in body,
            "AC1: RefPrimitive callee word recognised",
        )
        must(
            "closure_dispatch_leave_native()" in body,
            "AC1: refuse reuses the leave-native shape",
        )
        unlock = body.find("aura_unlock_workspace_read();")
        must(
            unlock >= 0 and arm > 0 and unlock < arm,
            "AC1: workspace read dropped before the arm",
        )
        echo = body.find("return closure_id; /* match IRInterpreter")
        must(
            echo >= 0 and arm < echo,
            "AC1: refuse arm precedes the legacy callee_val echo (the echo swallowed the write)",
        )

    # -- AC2: the driver re-runs the refused frame in the interpreter --
    must("Issue #4606" in svc, "AC2: driver TU cites #4606")
    svc_at = svc.find("[[nodiscard]] EvalResult exec_jit(")
    must(svc_at >= 0, "AC2: exec_jit entry present")
    if svc_at >= 0:
        sent = svc.find("aura_jit_take_prologue_deopt_sentinel() != 0", svc_at)
        must(sent >= 0, "AC2: exec_jit reads the #4084 sentinel")
        fb = svc.find("return eval_ir(input);", svc_at)
        must(fb >= 0, "AC2: exec_jit re-runs the restored IR in the interpreter")
        must(sent >= 0 and fb > sent, "AC2: the sentinel gates the interpreter fallback")

    # -- AC3: the write side keeps the #4057 / #4298 tenant stamps -----
    car_at, car = window(pair, 'add, ev, "set-car!"', 3600)
    must(car_at >= 0, "AC3: set-car! primitive registered")
    if car_at >= 0:
        must("#4298" in car, "AC3: set-car! keeps the #4298 in-range owner-stamp gate")
        must("#4057" in car, "AC3: set-car! keeps the #4057 process-level slot arm")
        must("g_pair_slot_tenants" in car, "AC3: set-car! reads the owner stamp array")
        must(
            "slot_tenant != ev.capability_tenant_id()" in car,
            "AC3: set-car! compares the slot owner against the caller principal",
        )
        must(
            "g_pair_slots[idx]->car = a[1].val;" in car,
            "AC3: set-car! writes the process-level PairSlot",
        )
    cdr_at, cdr = window(pair, 'add, ev, "set-cdr!"', 2800)
    must(cdr_at >= 0, "AC3: set-cdr! primitive registered")
    if cdr_at >= 0:
        must("#4298" in cdr, "AC3: set-cdr! keeps the #4298 in-range owner-stamp gate")
        must("#4057" in cdr, "AC3: set-cdr! keeps the #4057 process-level slot arm")
        must(
            "g_pair_slots[idx]->cdr = a[1].val;" in cdr,
            "AC3: set-cdr! writes the process-level PairSlot",
        )

    # -- AC4: the regression guard ------------------------------------
    must(
        'for mode in ("ir", "eval", "jit", "jitf1", "jitf2"):' in reg,
        "AC4: the 5-mode parity harness is present",
    )
    must(
        'if not (outs["ir"] == outs["eval"] == outs["jit"] == outs["jitf1"] == outs["jitf2"]):' in reg,
        "AC4: the 5-mode harness compares every mode",
    )
    try:
        tree = ast.parse(reg)
    except SyntaxError as exc:
        tree = ast.Module(body=[], type_ignores=[])
        must(False, f"AC4: tests/python/test_regression.py parses ({exc})")
    sound = _func(tree, "test_jit_soundness_4567")
    must(sound is not None, "AC4: the jit-soundness corpus function is present")
    if sound is not None:
        rows = _corpus_rows(sound)
        for name, program, expect in (
            (
                "pair-setcar-let-4602",
                "(define (g n) (let ((p (cons 1 2))) (set-car! p 9) (car p))) (display (g 1))",
                "9",
            ),
            (
                "pair-setcar-arg-4602",
                "(define (g p) (set-car! p 9) (car p)) (display (g (cons 1 2)))",
                "9",
            ),
            (
                "pair-setcdr-arg-4602",
                "(define (g p) (set-cdr! p 9) (cdr p)) (display (g (cons 1 2)))",
                "9",
            ),
            (
                "pair-setcar-twice-4602",
                "(define (g p) (set-car! p 9) (set-car! p 8) (car p)) (display (g (cons 1 2)))",
                "8",
            ),
        ):
            must(name in rows, f"AC4: corpus row {name} present")
            if name in rows:
                got_program, got_expect = rows[name]
                must(got_program == program, f"AC4: corpus row {name} carries the issue program")
                must(got_expect == expect, f"AC4: corpus row {name} expects {expect}")
    scalar = _func(tree, "test_jit_scalar_pairs_4573")
    must(scalar is not None, "AC4: the scalar-pairs corpus function is present")
    if scalar is not None:
        srows = _tuple_rows(scalar)
        must("set-car-escape" in srows, "AC4: the set-car-escape row is present")
        if "set-car-escape" in srows:
            vals = srows["set-car-escape"]
            must(
                vals[1] == "(define (sc x) (let ((p (cons x 2))) (set-car! p 9) (+ (car p) (cdr p)))) (display (sc 1))",
                "AC4: the set-car-escape row keeps its program",
            )
            must(vals[2] == "11", "AC4: the set-car-escape row expects 11 (car 9 + cdr 2)")
        must(
            not _compares_against(scalar, "name", "set-car-escape"),
            "AC4: the set-car-escape row is no longer excluded from the parity check",
        )
    tier = _func(tree, "test_jit_tier_differential_4574")
    must(tier is not None, "AC4: the tier-differential corpus function is present")
    if tier is not None:
        templates = _dict_entries(tier, "templates")
        must("pair-mutate" in templates, "AC4: the tier-differential corpus carries the pair-mutate template")
        if "pair-mutate" in templates:
            must(
                templates["pair-mutate"]
                == "(define (f n) (let ((p (cons n 2))) (set-car! p 9) (+ (car p) (cdr p)))) (display (f {n}))",
                "AC4: the pair-mutate template mutates then reads the pair",
            )

    # -- AC5: wiring ---------------------------------------------------
    must("check_jit_pair_mutation_4602.py" in build, "AC5: build.py registers this linter")
    must(
        "check_jit_pair_mutation_4602.py" in allow,
        "AC5: root_check_allowlist.txt lists this linter",
    )

    # -- AC6: house rules ----------------------------------------------
    must(not list(ROOT.glob("tests/**/test_issue_4602*.cpp")), "AC6: no dedicated test_issue_4602 file")
    must(not list(ROOT.glob("docs/design/4602-*")), "AC6: no docs/design/4602 markdown")

    if failures:
        print(f"\n{len(failures)} row(s) failed")
        for f in failures:
            print(f"  FAIL: {f}")
        return 1
    print("\nall rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
