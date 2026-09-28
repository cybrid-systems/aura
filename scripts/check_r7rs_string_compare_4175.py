#!/usr/bin/env python3
"""Issue #4175: R7RS string comparison suite — string>? / string>=? / string<=?.

Soft implemented string=? and string<? but left string>?, string>=?, and
string<=? unbound. R7RS requires the full comparison suite; the asymmetry
broke aura-build kernel modules using string>? tie-breaks
(aura/orch.aura:26 select-best → "unbound variable: string>?" → force-kernel
pursue fails).

A new prim must be wired through every registration layer in lockstep:
  AC1  ir.ixx SSOT: PrimId appends StringGt/StringGe/StringLe, kPrimNames
       gains the three names, size assert 44 -> 47 (append-only: existing
       numeric PrimId values stay stable for compiled artifacts)
  AC2  aura_jit.cpp lockstep: PrimStringGt/Ge/Le mirror enum (44/45/46) +
       exhaustive static_asserts (check_primid_drift_3270 contract)
  AC3  service.ixx kPrimNameTable mirror: JIT dispatch resolves PrimId ->
       name -> evaluator prim table
  AC4  lowering_impl.cpp prim_call_map: source names map to the new PrimIds
  AC5  type_checker_impl.cpp: typed (String, String) -> Bool signatures so
       the typed pipeline no longer reports unbound variable
  AC6  evaluator_primitives_pair.cpp: runtime registrations beside string<?
       with the #4175 cite (same shape: 2-arg minimum, lexicographic compare)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    ir = _read("src/compiler/ir.ixx")
    jit = _read("src/compiler/aura_jit.cpp")
    svc = _read("src/compiler/service.ixx")
    lowering = _read("src/compiler/lowering_impl.cpp")
    tc = _read("src/compiler/type_checker_impl.cpp")
    evp = _read("src/compiler/evaluator_primitives_pair.cpp")
    build = _read("build.py")

    # AC1 — ir.ixx SSOT: enum + kPrimNames + size assert.
    must("StringGt", "AC1 enum", ir)
    must("StringGe", "AC1 enum", ir)
    must("StringLe", "AC1 enum", ir)
    must('"string>?"', "AC1 kPrimNames", ir)
    must('"string>=?"', "AC1 kPrimNames", ir)
    must('"string<=?"', "AC1 kPrimNames", ir)
    must("std::size(kPrimNames) == 47", "AC1 size assert", ir)
    must("#4175", "AC1 cite", ir)

    # AC2 — aura_jit.cpp lockstep enum + drift asserts.
    must("PrimStringGt = 44", "AC2 enum", jit)
    must("PrimStringGe = 45", "AC2 enum", jit)
    must("PrimStringLe = 46", "AC2 enum", jit)
    must('static_assert(PrimStringGt == 44, "PrimId drift: aura_jit.cpp vs ir.ixx");', "AC2 assert", jit)
    must('static_assert(PrimStringGe == 45, "PrimId drift: aura_jit.cpp vs ir.ixx");', "AC2 assert", jit)
    must('static_assert(PrimStringLe == 46, "PrimId drift: aura_jit.cpp vs ir.ixx");', "AC2 assert", jit)

    # AC3 — service.ixx kPrimNameTable mirror (JIT PrimId -> name -> evaluator).
    must('"string>?"', "AC3 table", svc)
    must('"string>=?"', "AC3 table", svc)
    must('"string<=?"', "AC3 table", svc)
    must("#4175", "AC3 cite", svc)

    # AC4 — lowering prim_call_map coverage.
    must('{"string>?", PrimId::StringGt}', "AC4 map", lowering)
    must('{"string>=?", PrimId::StringGe}', "AC4 map", lowering)
    must('{"string<=?", PrimId::StringLe}', "AC4 map", lowering)
    must("#4175", "AC4 cite", lowering)

    # AC5 — type checker typed signatures.
    must('register_primitive("string>?", {String, String}, Bool);', "AC5 typecheck", tc)
    must('register_primitive("string>=?", {String, String}, Bool);', "AC5 typecheck", tc)
    must('register_primitive("string<=?", {String, String}, Bool);', "AC5 typecheck", tc)
    must("#4175", "AC5 cite", tc)

    # AC6 — evaluator runtime registrations (mirror string<? shape).
    must('add, ev, "string>?"', "AC6 register", evp)
    must('add, ev, "string>=?"', "AC6 register", evp)
    must('add, ev, "string<=?"', "AC6 register", evp)
    must("Lexicographic string greater-than.", "AC6 doc", evp)
    must("Lexicographic string greater-or-equal.", "AC6 doc", evp)
    must("Lexicographic string less-or-equal.", "AC6 doc", evp)
    must("#4175", "AC6 cite", evp)

    # Gate wiring — build.py row + allowlist row.
    must("check_r7rs_string_compare_4175", "wiring build.py", build)
    allow = _read("scripts/coverage/root_check_allowlist.txt")
    if "check_r7rs_string_compare_4175.py" not in allow:
        fails.append("wiring allowlist: check_r7rs_string_compare_4175.py not listed")

    if fails:
        print("FAIL #4175 r7rs_string_compare:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #4175 r7rs_string_compare: 6 AC layers wired (enum/jit/table/lowering/typecheck/eval)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
