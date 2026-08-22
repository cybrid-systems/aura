#!/usr/bin/env python3
"""Issue #3270: exhaustive PrimId drift asserts + OpGuardShape probe i1.

ir.ixx PrimId enum is the SSOT. aura_jit.cpp must static_assert every
entry (no 0-12 / 16-34 gaps). OpGuardShape captures linear_safety_probe()
i1 and ORs it into the epoch bb_stale branch. Quiet (no linear state)
returns nullptr — zero extra IR.

Contract:
  AC1  static_assert for every ir.ixx PrimId
  AC2  OpGuardShape uses lin_unsafe OR into bb_stale
  AC3  quiet nullptr; other sites still call probe; #3186 branch kept
  AC4  probe returns llvm::Value*; Capture/Apply still compile
  AC5  extend test_jit_macro_introduced_preserve; linter after #3269; no invent

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _parse_primid_enum(ir: str) -> list[tuple[str, int]]:
    start = ir.find("export enum class PrimId")
    if start < 0:
        return []
    brace = ir.find("{", start)
    end = ir.find("};", brace)
    body = ir[brace + 1 : end]
    names: list[tuple[str, int]] = []
    n = 0
    for raw in body.splitlines():
        line = raw.split("//", 1)[0].strip().rstrip(",")
        if not line:
            continue
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*(=\s*(\d+))?$", line)
        if not m:
            continue
        name = m.group(1)
        if m.group(3) is not None:
            n = int(m.group(3))
        names.append((name, n))
        n += 1
    return names


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ir = _read("src/compiler/ir.ixx")
    jit = _read("src/compiler/aura_jit.cpp")
    test = _read("tests/compiler/test_jit_macro_introduced_preserve.cpp")
    build = _read("build.py")
    l3269 = _read("scripts/coverage/checks/check_arena_compact_toctou_3269.py")

    prims = _parse_primid_enum(ir)
    if len(prims) < 40:
        fails.append(f"AC1: parsed {len(prims)} PrimId entries (expected 44)")
    must("Issue #3270: exhaustive", "AC1 cite", jit)
    for name, val in prims:
        needle = f"static_assert(Prim{name} == {val}"
        if needle not in jit:
            fails.append(f"AC1: missing {needle}")
    must("ac3270_1_exhaustive_primid_asserts", "AC1 test", test)

    gpos = jit.find("case OpGuardShape:")
    gwin = jit[gpos : gpos + 4000] if gpos >= 0 else ""
    must("auto* lin_unsafe = linear_safety_probe()", "AC2 capture", gwin)
    must("CreateOr(is_stale, lin_unsafe)", "AC2 OR stale", gwin)
    must("Issue #3270", "AC2 cite", gwin)
    must("ac3270_2_guardshape_uses_probe", "AC2 test", test)

    must("return nullptr; // Issue #3270: quiet path", "AC3 quiet", jit)
    must("linear_safety_probe();", "AC3 other sites", jit)
    must("CreateCondBr(any_unsafe, bb_deopt, bb_ok)", "AC3 #3186", jit)
    must("ac3270_3_quiet_and_other_sites", "AC3 test", test)

    must("auto linear_safety_probe = [&]() -> llvm::Value*", "AC4 return type", jit)
    must("return any_unsafe;", "AC4 return value", jit)

    must("ac3270_4_source_and_linter", "AC5 test", test)
    must("check_primid_drift_3270", "AC5 build.py", build)
    prev = build.find("check_arena_compact_toctou_3269")
    ours = build.find("check_primid_drift_3270")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3269")
    must("3270", "AC5 extend 3269 linter", l3269)
    if (ROOT / "tests" / "issues" / "test_issue_3270.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3270.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3270.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3270.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3270-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    q = _read("src/compiler/evaluator_primitives_query_tail.cpp")
    if "schema-3270" in q or "schema-3270" in test:
        fails.append("AC5: new schema-3270 query key (SlimSurface)")

    if fails:
        print("FAIL #3270 primid_drift:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print(f"OK #3270 primid_drift: {len(prims)} PrimId asserts + GuardShape probe")
    return 0


if __name__ == "__main__":
    sys.exit(main())
