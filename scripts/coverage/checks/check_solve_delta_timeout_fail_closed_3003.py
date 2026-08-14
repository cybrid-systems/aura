#!/usr/bin/env python3
"""Issue #3003: Production solve_delta fail-closed on TIMEOUT / partial.

Any Production solve_delta result that is not SOLVED must escalate
(#2277) then reject: no type write, no dirty-clear, no partial CS
stash, no query:type authority. Soft remains observe-only.

Contract:
  AC1 Production + TIMEOUT → escalate; not SOLVED → reject / fail-closed
  AC2 Soft TIMEOUT observe; no fail-closed bump
  AC3 last_type_export_authoritative + stash not live
  AC4 schema-3003; preserve #2277/#2913
  AC5 extend test_solve_delta_unresolved_export; linter; no docs/design /*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims  # Issue #2914

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    ixx = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    aud = _read("src/compiler/typed_mutation_audit.h")
    ev = _read("src/compiler/evaluator.ixx")
    tc = _read("src/compiler/evaluator_typecheck.cpp")
    q = read_query_prims()
    t = _read("tests/compiler/test_solve_delta_unresolved_export.cpp")
    build = _read("build.py")

    must("kDeltaTimeoutFailClosedIssue = 3003", "AC1", ixx)
    must("!forced_timeout_this_call_", "AC1 wrapper", impl)
    must("escalate_if_production", "AC1", impl)
    must("ac3003_1_production_solve_delta_fail_closed", "AC1", t)

    must("ac3003_2_soft_timeout_observe", "AC2", t)
    must("Soft / default Full-without-production_defaults", "AC2", impl)

    must("last_type_export_authoritative", "AC3", ixx)
    must("last_type_export_authoritative", "AC3 stash", tc)
    must("type_export_authoritative", "AC3", ev)
    must("not-authoritative", "AC3 query", _read("src/compiler/evaluator_primitives_eval.cpp"))
    must("ac3003_3_no_stash_no_authority", "AC3", t)

    must_key("schema-3003", "AC4", q)
    must_key("delta-timeout-fail-closed-total", "AC4", q)
    must("schema-2277", "AC4 lineage", q)
    must("delta_timeout_fail_closed_total", "AC4", aud)
    must("ac3003_4_schema_and_lineage", "AC4", t)

    must("ac3003_5_source_and_linter", "AC5", t)
    must("check_solve_delta_timeout_fail_closed_3003", "AC5", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3003.cpp").is_file():
        fails.append("AC5: test_issue_3003.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3003-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3003 Production solve_delta fail-closed — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
