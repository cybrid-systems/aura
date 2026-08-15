#!/usr/bin/env python3
"""Issue #3083: ADT exhaust complete seed after mutate.

After any ADT / pattern mutate that touches exhaustiveness, every match
whose subject type is in the mutated set must enter the reverify cone.
Production still hard-rejects residual under-mark (no Dynamic slide).
Soft observe-only. Empty types / no ADT → zero extra.

Contract (one row per AC):
  AC1 seed_adt_matches_for_dirty_types + collect_match_sites_for_adt_types
  AC2 Production TypeError + last_type_export_authoritative_ = false
  AC3 Soft observe only (no new hard fail)
  AC4 empty types → 0
  AC5 extend test_adt_match_goal_table; this linter; no invent / no design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims

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
    ev = _read("src/compiler/evaluator_typecheck.cpp")
    h = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = read_query_prims()
    test = _read("tests/compiler/test_adt_match_goal_table.cpp")
    build = _read("build.py")

    must("kAdtExhaustCompleteSeedIssue", "AC1", ixx)
    must("seed_adt_matches_for_dirty_types", "AC1 CS", ixx)
    must("seed_adt_matches_for_dirty_types", "AC1 impl", impl)
    must("collect_match_sites_for_adt_types", "AC1 collect", impl)
    must("note_adt_exhaust_dirty_type", "AC1 dirty-type", impl)
    must("Issue #3083", "AC1 force", impl)
    must("ac3083_1_complete_seed_incomplete_list", "AC1 test", test)

    must("last_type_export_authoritative_ = false", "AC2 authority", impl)
    must("adt_exhaust_production_reject_total", "AC2 reject", impl)
    must("adt_exhaust_dynamic_slide_prevented_total", "AC2 no slide", impl)
    must("Issue #3083", "AC2 evaluator", ev)
    must("ac3083_2_production_reject_retained", "AC2 test", test)

    must("adt_exhaust_soft_observe_total", "AC3 soft", impl)
    must("production_defaults_active()", "AC3 branch", impl)
    must("ac3083_3_soft_observe", "AC3 test", test)

    must("ac3083_4_quiet_empty", "AC4 test", test)
    if "if (adt_type_ids.empty())" not in impl and "if (types.empty())" not in impl:
        fails.append("AC4: empty-types early return missing")

    must_key("schema-3083", "AC5 schema", q)
    must_key("adt-exhaust-complete-seed-wired", "AC5 wired", q)
    must("adt_exhaust_complete_seed_total", "AC5 metrics", h)
    must("adt_exhaust_complete_seed_total", "AC5 fields", fields)
    must("schema-3045", "AC5 lineage 3045", q)
    must("ac3083_5_schema_and_linter", "AC5 test", test)
    must("check_adt_exhaust_complete_seed_3083", "AC5 build.py", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3083.cpp").is_file():
        fails.append("AC5: test_issue_3083.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3083-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3083 ADT exhaust complete seed — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
