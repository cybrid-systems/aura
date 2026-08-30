#!/usr/bin/env python3
"""Issue #3449: production query:* default export is schema-2, not opt-in.

#3395/#3286 auto-upgrade the finish helper; #3425 rejects as-stable-ref
raw-id. Residual: comments still advertised opt-in Agent memory, hash
OOM fell back to a green bare list, and :as-query-result #f was untested.

Contract:
  AC1 Production default find/pattern/filter/by-marker → schema-2 hash
  AC2 Production match count > 64 → query-result-overflow (reuse #3389)
  AC3 Soft/Off + no keyword → still a bare list
  AC4 :as-query-result #f under production is not a layout-only escape
  AC5 no new query key; no test_issue_3449.cpp; no docs/design/3449-*

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

    hh = _read("src/core/workspace_epoch.hh")
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    t = _read("tests/compiler/test_query_result_full_provenance.cpp")
    ep = _read("tests/compiler/test_query_epoch_contract.cpp")
    build = _read("build.py")

    must("kQueryDefaultSchema2ExportIssue", "AC1 stamp", hh)
    must("Issue #3449", "AC1 helper cite", qws)
    must("end_query_epoch_maybe_result", "AC1 finish helper", qws)
    must("as_query_result = true; // auto-upgrade", "AC1 #3395 retained", qws)
    must("query-result-tag", "AC1 hash tag", qws)
    must("test_3449_ac1_production_default_find_hash_to_mutate", "AC1 test", t)
    must("resolve_mutate_node_arg accepts default find hash", "AC1 mutate", t)

    must("query-result-overflow", "AC2 overflow reuse #3389", qws)
    must("g_query_result_full_provenance_total", "AC2 reuse #3103", hh)
    must("test_3449_ac2_production_overflow_no_keyword", "AC2 test", t)
    must_not("schema-3449", "AC2 no new query key", qws)
    must_not("schema-3449", "AC2 no schema in mutate", mut)

    must("Default (no keyword) remains the bare list", "AC3 Soft default string", qws)
    must("test_3449_ac3_soft_bare_list", "AC3 test", t)
    must("if (!as_query_result)", "AC3 Soft early return", qws)

    must("production QueryResult hash alloc failed", "AC4 OOM not bare list", qws)
    must("query-result-layout-only", "AC4 layout-only reject", qws)
    must("test_3449_ac4_prod_keyword_false_not_escape", "AC4 test", t)
    must(":as-query-result #f", "AC4 keyword-false test", t)

    must("check_query_default_schema2_export_3449", "AC5 build.py", build)
    must("check_query_default_stamped_3395", "AC5 #3395 retained", build)
    must("kQueryDefaultSchema2ExportIssue", "AC5 epoch suite", ep)
    must("Issue #3424", "AC5 #3424 retained", mut)
    must("Issue #3425", "AC5 #3425 retained", mut)
    prev = build.find("check_query_default_stamped_3395")
    ours = build.find("check_query_default_schema2_export_3449")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: #3449 linter must be wired after #3395")
    if (ROOT / "tests" / "compiler" / "test_issue_3449.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3449.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3449.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3449.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3449-*")):
            fails.append(f"AC5: docs/design/{f.name}")

    if fails:
        print("FAIL #3449 query_default_schema2_export:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3449 query_default_schema2_export: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
