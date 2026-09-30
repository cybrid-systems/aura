#!/usr/bin/env python3
"""Issue #4232: Soft cannot call car in soft_worldline_pick (host_fallback select).

On Soft tip a9ddd94 (--serve-async, Soft Ready denseness #4048), the product
brain's worldline select via helper aura/soft_worldline_pick.aura failed with
"type error: cannot call: car / did you forget to define 'car'?" and the host
fell back (soft_select.via=host_fallback). The exact text is produced only by
the tree-walker's prim-value call handler
(src/compiler/evaluator_eval_flat.cpp), where the callee PrimitiveRef —
manufactured from the full Evaluator registry by the unbound-Variable
fallback — was re-resolved through the ENV-level primitive table. Envs built
per the materialize_call_env contract (#145 Phase 2.3: primitives_/cells_/pool_
are caller-wired) can reach that handler without the wiring, so a registry-
known prim degraded to "cannot call". cdr/null? stayed reachable in the
product flow via env bindings, which is why the failure named car
specifically. The fix falls back to the same registry slot the prim value was
manufactured from (slot_lookup_fast) — bare prim names stay callable in any
eval env regardless of env-table wiring; user shadowing is unaffected.

Contract:
  AC1 prim-value call handler cites #4232 and carries the registry fallback
  AC2 runtime registrations present: car/cdr (pair prims) + null? (list prims)
  AC3 type-checker rows: car/cdr {Dyn}->Dyn and null? {Dyn}->Bool
  AC4 runtime ACs live in test_primcall_narg.cpp (#4232 AC helpers + calls)
  AC5 build.py wiring + root_check_allowlist entry; no docs/design/4232-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    evf = _read("src/compiler/evaluator_eval_flat.cpp")
    must("Issue #4232", "AC1", evf)
    must("prim = primitives_.slot_lookup_fast(slot);", "AC1", evf)
    must("eval_env.lookup_primitive(primitives_.name_for_slot(slot))", "AC1", evf)

    pairp = _read("src/compiler/evaluator_primitives_pair.cpp")
    must('"car"', "AC2", pairp)
    must('"cdr"', "AC2", pairp)
    must("(pair) -> any", "AC2", pairp)
    listp = _read("src/compiler/evaluator_primitives_list.cpp")
    must('"null?"', "AC2", listp)

    tc = _read("src/compiler/type_checker_impl.cpp")
    must('register_primitive("car", {Dyn}, Dyn)', "AC3", tc)
    must('register_primitive("cdr", {Dyn}, Dyn)', "AC3", tc)
    must('register_primitive("null?", {Dyn}, Bool)', "AC3", tc)
    must('env_.bind("car", reg_.register_forall', "AC3", tc)
    must('env_.bind("cdr", reg_.register_forall', "AC3", tc)

    test = _read("tests/compiler/test_primcall_narg.cpp")
    must("#4232", "AC4", test)
    must("eval_in_unwired_env", "AC4", test)
    must("ac17_unwired_env_list_prims", "AC4", test)
    must("ac18_soft_pick_best_unwired_env", "AC4", test)
    must("ac19_source_gate", "AC4", test)
    must("(car (quote (5 2 7)))", "AC4", test)

    build = _read("build.py")
    must("check_soft_list_prims_4232", "AC5", build)
    allow = _read("scripts/coverage/root_check_allowlist.txt")
    must("check_soft_list_prims_4232.py", "AC5", allow)
    if (ROOT / "docs" / "design").is_dir() and list((ROOT / "docs" / "design").glob("4232-*")):
        fails.append("AC5: docs/design/4232-* exists (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1
    print("OK: Issue #4232 soft worldline select list prims — AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
