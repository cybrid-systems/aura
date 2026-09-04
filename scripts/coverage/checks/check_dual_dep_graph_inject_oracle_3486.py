#!/usr/bin/env python3
"""Issue #3486: dual DepGraph lockless inject oracle.

#3165/#3187/#3255 write the production fail-closed path. Residual: no
adversarial one-sided inject that CI requires peel to survive.
Test-only string-only / NodeId-only hooks next to public_record_dependency.
Production peel-entry reuses #3255 helper. Rebuild stays
rebuild_node_dep_graph_from_string. No new query key.

Contract:
  AC1  inject_string_only_edge_for_test + NodeId drop; production peel
  AC2  cross-fiber record + inject; peel fail-closed
  AC3  Soft peel-entry skip (production gate); #3255 happy path stays
  AC4  rebuild_node_dep_graph_from_string remains authority
  AC5  extend test_dep_graph_hybrid_cascade; no invent / docs / new query
  AC6  linter AFTER #3255

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

    ixx = _read("src/compiler/service.ixx")
    dirty = _read("src/compiler/dirty_propagation.ixx")
    t = _read("tests/compiler/test_dep_graph_hybrid_cascade.cpp")
    build = _read("build.py")

    must("inject_string_only_edge_for_test", "AC1 string inject", ixx)
    must("inject_node_only_edge_for_test", "AC1 node inject", ixx)
    must("Issue #3486", "AC1 cite", ixx)
    inj = ixx.find("void inject_string_only_edge_for_test")
    ninj = ixx.find("void inject_node_only_edge_for_test")
    iwin = ixx[inj:ninj] if inj >= 0 and ninj > inj else (ixx[inj : inj + 700] if inj >= 0 else "")
    must("ensure_dep_fn_slot_", "AC1 slots so parity sees fork", iwin)
    if "mirror_fn_dep_edge_unlocked_" in iwin:
        fails.append("AC1: string-only inject must not call mirror_fn_dep_edge_unlocked_")
    nwin = ixx[ninj : ninj + 700] if ninj >= 0 else ""
    must("mirror_fn_dep_edge_unlocked_", "AC1 node-only mirrors", nwin)

    rel = ixx.find("std::size_t relower_dirty_defines_from_workspace()")
    peel = ixx[rel : rel + 20000] if rel >= 0 else ""
    must("Issue #3486", "AC1 peel cite", peel)
    must("fail_closed_soft_dual_graph_parity_before_partial_", "AC1 reuses #3255", peel)
    b3486 = peel.find("Issue #3486")
    block = peel[b3486 : b3486 + 2500] if b3486 >= 0 else ""
    must("production_defaults_active()", "AC3 production gate", block)

    must("rebuild_node_dep_graph_from_string", "AC4 rebuild", dirty)
    must("rebuild_node_dep_graph_from_string", "AC4 peel/helper", ixx)

    must("ac3486_1_string_only_inject_production", "AC1 test", t)
    must("3486 AC1: lookup_define_v2(x3486) is not silent clean", "AC1 lookup", t)
    must("ac3486_1b_node_drop_production", "AC1 node drop", t)
    must("ac3486_2_cross_fiber", "AC2 cross-fiber", t)
    must("ac3486_3_soft_zero_extra", "AC3 Soft", t)
    must("ac3255_1_soft_fork_forces_full", "AC5 #3255 stays", t)
    must("ac3187_production_fail_closed_default", "AC5 #3187 stays", t)
    must("ac3165_strict_fail_closed_all_callers", "AC5 #3165 stays", t)

    must("check_dual_dep_graph_inject_oracle_3486", "AC5 build.py", build)
    prev = build.find("check_dual_dep_graph_soft_parity_partial_3255")
    ours = build.find("check_dual_dep_graph_inject_oracle_3486")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC6: linter must be wired in build.py AFTER #3255")

    must_not("schema-3486", "AC5 no schema-3486", ixx)
    must_not("g_3486_", "AC5 no g_3486_*", ixx)
    must_not("query:dual-graph-inject", "AC5 no new query", ixx)
    if (ROOT / "tests" / "compiler" / "test_issue_3486.cpp").is_file():
        fails.append("AC5: test_issue_3486.cpp present")
    if (ROOT / "tests" / "issues" / "test_issue_3486.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3486.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3486-*")):
            fails.append(f"AC5: docs/design/{f.name} present")

    if fails:
        print("FAIL #3486 dual_dep_graph_inject_oracle:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3486 dual_dep_graph_inject_oracle: inject hooks; peel fail-closed; string authority")
    return 0


if __name__ == "__main__":
    sys.exit(main())
