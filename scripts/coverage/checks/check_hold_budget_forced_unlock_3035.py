#!/usr/bin/env python3
"""Issue #3035: force unlock + dual-topology restore on hold-budget cancel
for a non-yield body.

Residual of #2999: consuming the cancel flag at dtor/safepoint edges
fail-closes the Guard, but a mutate body that never polls the flag can
keep workspace_mtx_ + per-fiber depth slot held indefinitely (steal/GC
residual starve; densify×steal can observe half-topology). #3035 closes
the remaining window: when the outermost Guard dtor consumes a pending
hold-budget cancel under production / hard-env, the boundary is forced
fail-closed (success=false EVEN when the Guard carries no success flag)
so exit_mutation_boundary runs abort_restore_dual_topology + dual canary
(same path as panic/abort) and the exit pipeline force-releases
workspace_mtx_ + clears the fiber depth slot + closes residual defer.

Contract (one row per AC):
  AC1 Production: cancel consume forces success=false + forced-unlock
     counter + dual restore path; residual_defer == 0.
  AC2 Soft / sandbox=off: observe only; no consume / no force-unlock.
  AC3 Happy path (no cancel): zero extra stores (counter unchanged).
  AC4 Additive: mutation-hold-budget-forced-unlock-total; schema-3035 /
     issue-3035; #2999/#2932/#2726 preserved.
  AC5 Extend hold-starvation / chaos residual_zero (#81967); linter.
  AC6 Source-cite + linter; no docs/design/* per #1655.

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

    mhb = _read("src/compiler/mutation_hold_budget.h")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    _read("src/compiler/evaluator_fiber_mutation.cpp")
    q = read_query_prims()
    t = _read("tests/serve/test_mailbox_hold_starvation_hard.cpp")
    chaos = _read("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp")
    build = _read("build.py")

    must("Issue #3035", "AC1", emb)
    must("cancel_forced_fail", "AC1 force-fail on cancel consume", emb)
    must("forced_unlock_total", "AC1 forced-unlock counter bump", emb)
    must("abort_restore_dual_topology", "AC1 dual restore path", emb)
    must("kMutationHoldBudgetForcedUnlockIssue = 3035", "AC1", mhb)
    must("success = cancel_forced_fail ? false", "AC1 flag-null force-fail", emb)

    must("peek_hold_budget_cancel", "AC2 Soft peek", emb)
    must("soft_observe_total", "AC2 Soft observe", emb)
    must("mutation_hold_budget_reject_enabled()", "AC2", emb)

    must("is_outermost_", "AC3 outermost-only", emb)

    must_key("mutation-hold-budget-forced-unlock-total", "AC4", q)
    must_key("mutation-hold-budget-forced-unlock-wired", "AC4", q)
    must_key("schema-3035", "AC4", q)
    must_key("issue-3035", "AC4", q)
    must_key("schema-2999", "AC4 preserved", q)
    must_key("schema-2932", "AC4 preserved", q)
    must_key("schema-2726", "AC4 preserved", q)
    must("ac3035_1_forced_unlock_dual_restore", "AC5", t)
    must("ac3035_2_soft_metric_only", "AC5", t)
    must("ac3035_3_happy_path_zero_force", "AC5", t)
    must("ac3035_4_query_keys", "AC5", t)
    must("ac3035_residual_force_unlock_cite", "AC5 chaos residual_zero", chaos)
    must("check_hold_budget_forced_unlock_3035", "AC5", build)
    if (ROOT / "tests" / "serve" / "test_issue_3035.cpp").is_file():
        fails.append("AC5: tests/serve/test_issue_3035.cpp present (forbidden invent)")

    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("3035-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3035 hold-budget forced unlock — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
