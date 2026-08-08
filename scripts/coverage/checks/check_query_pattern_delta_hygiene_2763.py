#!/usr/bin/env python3
"""Issue #2763: query:pattern index default delta rebuild + MacroIntroduced hygiene hard filter.

Refines closed #1503 (delta/full threshold) + #2123 (default MacroIntroduced
hygiene). Production multi-round AI edit loops need:
  - low dirty ratio → delta rebuild (no full walk) with Agent counters
  - MacroIntroduced hard-filtered unless query opts in
Soft/sandbox stay ergonomic; additive observability only.

Contract (one row per AC):
  AC1 evaluator_query_index delta under low dirty + bump_query_pattern_delta_rebuild
  AC2 query_matcher skip_macro_introduced hard filter (#2763 cite)
  AC3 Soft/opt-in include-macro path preserved
  AC4 Quiet already-synced early return (zero rebuild)
  AC5 Additive query-pattern-delta-rebuild-total + hygiene-filtered + schema-2763;
     schema 1503/2123 preserved; no docs/design/*
  AC6 Extend test_query_pattern_default_hygiene.cpp; this linter wired.

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

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    idx = _read("src/compiler/evaluator_query_index.cpp")
    eix = _read("src/compiler/evaluator.ixx")
    met = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    matcher = _read("src/compiler/query_matcher.ixx")
    mcpp = _read("src/compiler/query_matcher.cpp")
    t = _read("tests/compiler/test_query_pattern_default_hygiene.cpp")
    build = _read("build.py")

    # AC1 — delta default under low dirty ratio.
    must("#2763", "AC1", idx)
    must("bump_query_pattern_delta_rebuild", "AC1", idx)
    must("bump_query_pattern_full_rebuild", "AC1", idx)
    must("tag_arity_index_sync_after_mutation", "AC1", idx)
    must("bump_query_pattern_delta_rebuild", "AC1", eix)
    must("query_pattern_delta_rebuild_total", "AC1", met)

    # AC2 — MacroIntroduced hard filter.
    must("#2763", "AC2", matcher + mcpp)
    must("skip_macro_introduced", "AC2", matcher + mcpp)
    must("is_macro_introduced", "AC2", mcpp)
    must("#2763", "AC2", qws)

    # AC3 — Soft/opt-in preserved (include keywords still live).
    must("include-macro-introduced", "AC3", qws)
    must("allow-macro-introduced", "AC3", qws)
    must("pattern_include_macro_opt_in_total", "AC3", met)

    # AC4 — quiet already-synced path.
    must("tag_arity_index_synced_size_", "AC4", idx)
    must("tag_arity_index_synced_gen_", "AC4", idx)

    # AC5 — additive observability + prior surfaces.
    must("query_pattern_delta_rebuild_total", "AC5", met)
    must("query_pattern_full_rebuild_total", "AC5", met)
    must_key("query-pattern-delta-rebuild-total", "AC5", q)
    must_key("query-pattern-full-rebuild-total", "AC5", q)
    must_key("query-pattern-hygiene-filtered-total", "AC5", q)
    must_key("schema-2763", "AC5", q)
    must_key("issue-2763", "AC5", q)
    must("1503", "AC5", q)  # schema lineage
    must("2123", "AC5", q)  # hygiene lineage
    must("pattern_hygiene_filtered_total", "AC5", met)

    # AC6 — tests + linter + no docs/design + no forbidden test_issue file.
    must("ac2763_1_delta_rebuild_low_dirty", "AC6", t)
    must("ac2763_2_macro_hard_filter", "AC6", t)
    must("ac2763_3_soft_opt_in", "AC6", t)
    must("ac2763_4_quiet_path", "AC6", t)
    must("ac2763_5_observability", "AC6", t)
    must("ac2763_6_source_and_linter", "AC6", t)
    must("check_query_pattern_delta_hygiene_2763", "AC6", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2763.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_2763.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2763-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2763 query:pattern delta rebuild + MacroIntroduced hygiene hard filter — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
