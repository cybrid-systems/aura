#!/usr/bin/env python3
"""Issue #3118: production hold-budget cancel force-unlock + depth clear.

Residual of #3035/#3071: dtor consume fail-closes, but lock + depth stay
held through Phase-1-4. Production cancel must drop workspace_mtx_ and
the fiber depth slot immediately after abort_restore_dual_topology.
Soft/sandbox=off stays observe-only. No new query keys.

Contract:
  AC1 Production cancel consume → force-release lock + depth==0 after restore
  AC2 Dual-topology restore still runs (canary / children_column_restored)
  AC3 Soft: observe-only, no force-release
  AC4 Happy path: no extra work beyond existing peek
  AC5 Existing hold-budget / residual counters non-regressing
  AC6 Source-cite + extend existing tests; no test_issue_3118; no docs/design

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

    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    mhb = _read("src/compiler/mutation_hold_budget.h")
    t = _read("tests/serve/test_mailbox_hold_starvation_hard.cpp")
    ht = _read("tests/compiler/test_mutation_hold_hard_timeout.cpp")
    chaos = _read("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp")
    build = _read("build.py")
    q = read_query_prims() + _read("src/compiler/evaluator_primitives_query_type_stats.cpp")

    must("Issue #3118", "AC1 emb", emb)
    must("force_release_hold_after_cancel_", "AC1 helper", emb)
    must("cancel_forced_fail && outermost", "AC1 after restore", emb)
    must("cancel_force_released_", "AC1 skip flag", ev)
    must("kMutationHoldBudgetCancelForceReleaseIssue = 3118", "AC1 stamp", mhb)
    must("3118 AC1", "AC1 test", t)

    must("abort_restore_dual_topology", "AC2 dual restore", emb)
    must("3118 AC2", "AC2 test", t)

    must("peek_hold_budget_cancel", "AC3 Soft peek", emb)
    must("soft_observe_total", "AC3 Soft observe", emb)
    must("3118 AC3", "AC3 test", t)

    must("cancel_forced_fail && outermost", "AC4 gated", emb)
    must("ac3118_source_cite", "AC4 hard-timeout", ht)

    must("forced_unlock_total", "AC5 reuse 3035 counter", emb)
    must("check_hold_budget_forced_unlock_3035", "AC5 3035 preserved", build)
    must("check_hold_budget_inbody_window_3071", "AC5 3071 preserved", build)

    must("check_hold_budget_cancel_force_release_3118", "AC6 build.py", build)
    must("ac3118_residual_force_release_cite", "AC6 chaos", chaos)
    if "schema-3118" in q:
        fails.append("AC6: new query key schema-3118 (forbidden)")
    if (ROOT / "tests" / "compiler" / "test_issue_3118.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_3118.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "serve" / "test_issue_3118.cpp").is_file():
        fails.append("AC6: tests/serve/test_issue_3118.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3118-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3118 hold-budget cancel force-release — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
