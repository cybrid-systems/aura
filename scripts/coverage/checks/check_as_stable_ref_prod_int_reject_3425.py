#!/usr/bin/env python3
"""Issue #3425: query:as-stable-ref rejects bare int under production.

#3395 closed occupancy on resolve helpers; this prim was the remaining
Agent converter that restamped whatever currently occupies the index.
Production inbound is packed v2 / schema-2 QueryResult; Soft int → v1
unchanged.

Contract:
  AC1 production as-stable-ref does not as_int(a[0]) → make_ref_layout
  AC2 production + int → stale-ref; live v2/hash → v2 pair; stale v2 → stale-ref
  AC3 Soft int → v1 pair unchanged
  AC4 #3398 v2 packer, #3396 walk_v2, #3230 torn gate non-regress
  AC5 tests in test_stable_ref_provenance_fiber_cow; linter after #3398;
      no docs/design/3425-*; no test_issue_3425.cpp

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

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    t = _read("tests/serve/test_stable_ref_provenance_fiber_cow.cpp")
    build = _read("build.py")

    start = mut.find('add("query:as-stable-ref"')
    win = mut[start : start + 9000] if start >= 0 else ""
    must("Issue #3425", "AC1 cite", win)
    must("raw node-id rejected under production", "AC1 reject", win)
    must("is_int(a[0])", "AC1 int gate", win)
    must("production_defaults_active()", "AC1 production gate", win)
    if "make_ref_layout" in win:
        fails.append("AC1: production as-stable-ref must not call make_ref_layout")
    rej = win.find("raw node-id rejected under production")
    exp = win.find("export_ref(")
    if rej < 0 or exp < 0 or rej > exp:
        fails.append("AC1: production int reject must run before export_ref occupancy remake")

    must("auto_refresh=*/false", "AC2 no occupancy auto-refresh", win)
    must("resolve_query_result_match", "AC2 hash path", win)
    must("unpack_stable_ref_arg", "AC2 v2 unpack", win)
    must("stale-ref", "AC2 stale kind", win)

    must("Soft (or sandbox=off): historical v1 (id . gen) pair", "AC3 Soft v1", win)
    must("make_int(static_cast<std::int64_t>(ref.id))", "AC3 Soft id", win)
    must("make_int(static_cast<std::int64_t>(ref.gen))", "AC3 Soft gen", win)

    must("Issue #3398: v2 spine packer", "AC4 #3398 packer", mut)
    must("walk_v2", "AC4 #3396", mut)
    must("Issue #3230", "AC4 #3230 torn gate", win)

    must("ac3425_1_source_cite", "AC5 test", t)
    must("ac3425_2_production_int_reject_v2_and_hash", "AC5 AC2 test", t)
    must("ac3425_3_soft_int_v1_unchanged", "AC5 AC3 test", t)
    must("check_as_stable_ref_prod_int_reject_3425", "AC5 build.py", build)
    prev = build.find("check_as_stable_ref_v2_3398")
    ours = build.find("check_as_stable_ref_prod_int_reject_3425")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: #3425 linter must run after #3398")
    if (ROOT / "tests" / "issues" / "test_issue_3425.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3425.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3425.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3425.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3425-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3425 as_stable_ref_prod_int_reject:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3425 as_stable_ref_prod_int_reject: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
