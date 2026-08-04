#!/usr/bin/env python3
"""Issue #2482: is_end_of_list / null? / list empty use void only (not int 0).

Contract:
  AC1 is_end_of_list void-only in list + runtime
  AC2 null? void-only
  AC3 list lowering ConstVoid (no ConstI64 empty)
  AC4 PrimNullP void-only (no is_zero)
  AC5 gate wiring

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


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
            fails.append(f"{label}: unexpected {n!r}")

    lst = _read("src/compiler/evaluator_primitives_list.cpp")
    rt = _read("src/compiler/evaluator_primitives_runtime.cpp")
    low = _read("src/compiler/lowering_impl.cpp")
    jit = _read("src/compiler/aura_jit.cpp")
    test = _read("tests/compiler/test_list_end_of_list_void_2482.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    eidx = lst.find("bool is_end_of_list")
    ebody = lst[eidx : eidx + 350] if eidx >= 0 else ""
    must("Issue #2482", "AC1", lst)
    must("is_void(v)", "AC1", ebody)
    must_not("as_int(v) == 0", "AC1", ebody)

    nidx = lst.find('add("null?"')
    nbody = lst[nidx : nidx + 250] if nidx >= 0 else ""
    must("is_void", "AC2", nbody)
    must_not("as_int", "AC2", nbody)

    reidx = rt.find("bool is_end_of_list")
    rbody = rt[reidx : reidx + 350] if reidx >= 0 else ""
    must("Issue #2482", "AC1-rt", rt)
    must("is_void(v)", "AC1-rt", rbody)
    must_not("as_int(v) == 0", "AC1-rt", rbody)

    # Comment with #2482 sits just above callee_name == "list"
    lidx = low.find("Expand (list a b c)")
    if lidx < 0:
        lidx = low.find('callee_name == "list"')
    lbody = low[lidx : lidx + 1100] if lidx >= 0 else ""
    must("Issue #2482", "AC3", lbody)
    must("ConstVoid", "AC3", lbody)
    must_not("ConstI64", "AC3", lbody)

    jidx = jit.find("case PrimNullP:")
    jbody = jit[jidx : jidx + 500] if jidx >= 0 else ""
    must("Issue #2482", "AC4", jbody)
    must_not("is_zero", "AC4", jbody)
    must_not("CreateOr", "AC4", jbody)

    must("2482 AC1", "AC5", test)
    must("check_list_end_of_list_void_2482", "gate", build)
    must("cmd_list_end_of_list_void_coverage", "gate", build)
    must("test_list_end_of_list_void_2482", "gate", cmake)
    must("2482 AC5", "gate", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: list end-of-list void-only #2482 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
