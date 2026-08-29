#!/usr/bin/env python3
"""Issue #3424: resolve_*_node_arg unpack schema-2 QueryResult hash.

Production query:* auto-upgrades to a hash (#3395/#3286). The helpers
must decode is_hash + query_result_is_fresh_with_refs instead of
forcing Agent occupancy (hash → int → make_stamped_ref).

Contract:
  AC1 both resolve helpers (or shared resolve_query_result_match)
      contain is_hash + query_result_is_fresh_with_refs
  AC2 production query:find hash is a legal node operand
  AC3 Soft bare list / int path unchanged
  AC4 #3395 bare-int reject + #3396 v2 pair + #3286 auto-upgrade
  AC5 tests in test_query_result_full_provenance; linter after #3399;
      no docs/design/3424-*; no test_issue_3424.cpp

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
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    dec = _read("src/compiler/query_result_decode.hh")
    t = _read("tests/compiler/test_query_result_full_provenance.cpp")
    build = _read("build.py")

    must("kQueryResultHashResolveIssue = 3424", "AC1 stamp", dec)
    must("resolve_query_result_match", "AC1 shared helper", dec)
    must("query_result_is_fresh_with_refs", "AC1 freshness", dec)
    must("is_hash", "AC1 decode is_hash", dec)
    if "make_stamped_ref" in dec:
        fails.append("AC1: hash decode must not call make_stamped_ref (occupancy)")

    mut_res = mut.find("auto resolve_mutate_node_arg")
    mut_win = mut[mut_res : mut_res + 2500] if mut_res >= 0 else ""
    must("is_hash(arg)", "AC1 mutate is_hash", mut_win)
    must("resolve_query_result_match", "AC1 mutate shared", mut_win)
    must("query_result_is_fresh_with_refs", "AC1 mutate freshness cite", mut)

    q_res = qws.find("auto resolve_query_node_arg")
    q_win = qws[q_res : q_res + 2500] if q_res >= 0 else ""
    must("is_hash(arg)", "AC1 query is_hash", q_win)
    must("resolve_query_result_match", "AC1 query shared", q_win)
    must("query_result_is_fresh_with_refs", "AC1 query freshness", qws)

    must("test_3424_ac1_source_cite", "AC5 test", t)
    must("test_3424_ac2_production_hash_to_mutate", "AC2 test", t)
    must("raw node-id rejected under production", "AC4 #3395 mutate", mut)
    must("raw node-id rejected under production", "AC4 #3395 query", qws)
    must("walk_v2", "AC4 #3396", mut)
    must("as_query_result = true; // auto-upgrade", "AC4 #3286", qws)

    must("check_query_result_hash_resolve_3424", "AC5 build.py", build)
    prev = build.find("check_structural_mutate_resolve_helper_3399")
    ours = build.find("check_query_result_hash_resolve_3424")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: #3424 linter must run after #3399")
    if (ROOT / "tests" / "issues" / "test_issue_3424.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3424.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3424.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3424.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3424-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3424 query_result_hash_resolve:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3424 query_result_hash_resolve: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
