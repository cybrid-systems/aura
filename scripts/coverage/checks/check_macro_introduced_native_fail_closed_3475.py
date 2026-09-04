#!/usr/bin/env python3
"""Issue #3475: MacroIntroduced must not run as User native under production.

#1610/#2022/#2100/#2764 stamp source_marker and deopt on dirty∧MacroIntroduced.
Lost dirty / generation still compiled ordinary opcodes. Production now
MustDeopt / refuses native install when the envelope is MacroIntroduced
and dirty/provenance is inconsistent. Ancestor walk cap 64 fail-closes
instead of emitting User. Soft/Off keep consult + dirty-only deopt.

Contract:
  AC1  production lower + get_function_ptr refuse native; helper armed
  AC3  Soft/Off helper returns 0; dirty∧MacroIntroduced deopt kept
  AC4  ancestor cap: production treats unwalked prefix as envelope
  AC5  #2764 propagate + #1610 consult kept; no new query key
  AC6  extend test_jit_macro_deopt_hygiene; no invent / docs/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    jit = _read("src/compiler/aura_jit.cpp")
    low = _read("src/compiler/lowering.ixx")
    rt = _read("src/compiler/aura_jit_runtime.cpp")
    test = _read("tests/compiler/test_jit_macro_deopt_hygiene.cpp")
    preserve = _read("tests/compiler/test_jit_macro_introduced_preserve.cpp")
    build = _read("build.py")

    must("aura_macro_hygiene_production_fail_closed", "AC1 helper def", rt)
    must("Issue #3475", "AC1 runtime cite", rt)
    must("production_defaults_active", "AC1 production gate", rt)
    must("AuditStrategy::Full", "AC1 Full gate", rt)

    must("Issue #3475", "AC1 jit cite", jit)
    must("aura_macro_hygiene_production_fail_closed", "AC1 jit consult", jit)
    must("aura_jit_macro_hygiene_consult_inc", "AC3 consult kept", jit)
    must("inst.dirty != 0", "AC3 dirty deopt kept", jit)
    low_fn = jit.find("if (inst.source_marker == 1 /*MacroIntroduced*/ || fn.source_marker == 1)")
    if low_fn < 0:
        fails.append("AC1: lower() must consult function envelope as well as inst marker")
    else:
        win = jit[low_fn : low_fn + 1800]
        must("aura_macro_hygiene_production_fail_closed", "AC1 lower fail-closed", win)
        must("aura_jit_macro_introduced_deopt_inc", "AC1 lower deopt", win)
        if "return false" not in win:
            fails.append("AC1: lower() must refuse native (return false)")

    gp = jit.find("void* get_function_ptr(const char* name")
    gp_win = jit[gp : gp + 2200] if gp >= 0 else ""
    must("source_marker == 1", "AC1 lookup marker", gp_win)
    must("aura_macro_hygiene_production_fail_closed", "AC1 lookup fail-closed", gp_win)

    must("Issue #3475", "AC4 lowering cite", low)
    must("kMaxAncestorWalk", "AC4 cap kept", low)
    must("aura_macro_hygiene_production_fail_closed", "AC4 cap gate", low)
    cap = low.find("d == kMaxAncestorWalk")
    if cap < 0:
        fails.append("AC4: ancestor cap must fail-closed when unwalked parents remain")
    else:
        cwin = low[cap : cap + 900]
        must("SyntaxMarker::MacroIntroduced", "AC4 cap returns envelope", cwin)

    must("propagate_marker_from_ast", "AC5 #2764 helper", low)
    must("aura_jit_stamp_fn_macro_marker", "AC5 #2022 stamp", jit)
    must_not("schema-3475", "AC6 no query key", jit + low + rt)
    must_not("g_3475_", "AC6 no g_3475_*", jit + low + rt)

    must("3475 AC1: production refuses native install", "AC6 live refuse", test)
    must("3475 AC3: Soft/Off helper is zero extra", "AC6 Soft", test)
    must("ac3475_production_macro_refuse_native", "AC6 test fn", test)
    must("Issue #2022", "AC5 preserve suite", preserve)

    must("check_macro_introduced_native_fail_closed_3475", "AC6 build.py", build)
    prev = build.find("check_ir_jit_macro_marker_enforcement_2764")
    ours = build.find("check_macro_introduced_native_fail_closed_3475")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC6: linter must be wired in build.py AFTER #2764")

    if (ROOT / "tests" / "compiler" / "test_issue_3475.cpp").is_file():
        fails.append("AC6: test_issue_3475.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3475-*")):
            fails.append(f"AC6: docs/design/{f.name} present")

    if fails:
        print("FAIL #3475 macro_introduced_native_fail_closed:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3475 macro_introduced_native_fail_closed: production refuse native; Soft dirty-only")
    return 0


if __name__ == "__main__":
    sys.exit(main())
