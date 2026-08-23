#!/usr/bin/env python3
"""Issue #3019: unified restamp after boundary / abort / steal / densify.

Contract (one row per AC):
  AC1  unified_restamp_after_boundary is the shared entry for Boundary
       success, abort restore, steal complete, and densify.
  AC2  Order is node gen → StableNodeRef → LifetimePin (documented).
  AC3  Budget exceed marks torn visible (additive schema-3019); restamp-lag
       / budget keys are not renamed.
  AC4  Soft steal/densify with no wrap and no last-budget-exceeded skip
       extra node/pin walks.
  AC5  Extend test_restamp_sla_observability (#81967); no test_issue_3019.cpp;
       no docs/design/ (#1655).

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

    ev = _read("src/compiler/evaluator.ixx")
    fm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    gc = _read("src/compiler/evaluator_gc.cpp")
    restamp = _read("src/core/flatast_restamp.hh")
    ast = _read("src/core/ast.ixx")
    review = _read("src/compiler/evaluator_primitives_stdlib_review.cpp")
    test = _read("tests/core/test_restamp_sla_observability.cpp")
    build = _read("build.py")

    must("unified_restamp_after_boundary", "AC1", ev)
    must("UnifiedRestampSite", "AC1", ev)
    must("BoundarySuccess", "AC1", ev)
    must("AbortRestore", "AC1", ev)
    must("StealComplete", "AC1", ev)
    must("Densify", "AC1", ev)
    must("UnifiedRestampSite::BoundarySuccess", "AC1 mb", mb)
    must("UnifiedRestampSite::AbortRestore", "AC1 mb", mb)
    must("UnifiedRestampSite::StealComplete", "AC1 steal", fm)
    must("UnifiedRestampSite::Densify", "AC1 densify", fm)
    must("UnifiedRestampSite::StealComplete", "AC1 gc", gc)

    uni = fm.find("Evaluator::unified_restamp_after_boundary")
    if uni < 0:
        fails.append("AC2: unified impl missing")
    else:
        # Full-triad path starts after the Soft skip return.
        start = fm.find("if (ws) {", uni)
        body = fm[start : start + 2800] if start > 0 else ""
        n = body.find("restamp_all_node_generations")
        s = body.find("auto_restamp_pinned_stable_refs_at")
        p = body.find("restamp_all_pins_for_arena")
        if n < 0 or s < 0 or p < 0 or not (n < s < p):
            fails.append("AC2: order must be node gen → stable → pin")
    must("stables/pins must observe", "AC2 doc", fm)

    must("kUnifiedRestampIssue = 3019", "AC3", restamp)
    must("g_unified_restamp_torn_visible_total", "AC3", restamp)
    must("schema-3019", "AC3", review)
    must("unified-restamp-torn-visible-total", "AC3", review)
    must("schema-3000", "AC3 lag preserved", review)
    must("schema-2934", "AC3 budget preserved", review)
    must("kUnifiedRestampIssue", "AC3 ast export", ast)

    must("skipped_extra = true", "AC4", fm)
    must("!production && !wrap_pending && !last_budget", "AC4", fm)

    must("ac3019_1_unified_entry_four_sites", "AC5", test)
    must("ac3019_5_canary_soak_linter", "AC5", test)
    must("check_unified_restamp_3019", "AC5", build)
    must("cmd_unified_restamp_3019", "AC5", build)
    if _read("tests/core/test_issue_3019.cpp"):
        fails.append("AC5: test_issue_3019.cpp exists — forbidden per #81967")
    if _read("docs/design/3019-unified-restamp.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")
    if "AgentRegistry" in fm[fm.find("Issue #3019") : fm.find("Issue #3019") + 800]:
        fails.append("AC5: must not introduce AgentRegistry")

    if fails:
        print(f"Issue #3019 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3019 unified restamp — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
