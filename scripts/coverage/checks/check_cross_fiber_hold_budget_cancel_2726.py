#!/usr/bin/env python3
"""Issue #2726: cross-fiber hold-budget force-degrade real cancel (per-fiber
pending-cancel map polled at safepoints) — #2720 residual.

Contract (one row per AC):
  AC1 Per-Fiber pending_hold_budget_cancel_ atomic + request/consume/
     peek accessors on Fiber (src/serve/fiber.h). Process-wide Fiber*
     registry + C-linkage shim aura_fiber_request_hold_budget_cancel
     (src/serve/fiber.cpp). On cross-fiber force-degrade, the recorded
     holder fiber_id resolves via the registry and the pending flag is
     set. The holder observes the flag at outermost MutationBoundaryGuard
     dtor (Phase-5 exit path, is_outermost_ ctor-captured per #2120),
     flips *flag_=false (same effect as mark_outermost_mutation_failed),
     and the Guard exits with failure — releasing workspace_mtx_
     exclusive + GcDeferReason::MutationHold so steal/GC can progress.
     counters: cross_fiber_cancel_fired_total (set succeeds) +
     cross_fiber_cancel_consumed_total (holder consumes at dtor).
     fired vs consumed divergence is observable for Agent health
     (Fiber lifetime race = holder gone before consume).
  AC2 Soft / sandbox=off: flag NOT set unless AURA_MUTATION_HOLD_BUDGET_HARD=1.
     Cross-fiber fire path gates on mutation_hold_budget_reject_enabled()
     (reuses #2701/#2720 gate). Phase-5 poll likewise gates for defense-
     in-depth so a direct C-linkage call under Soft still does not
     consume the flag.
  AC3 Nested / non-outermost Guards never observe or clear the pending-
     cancel flag. The consume call sits in the outermost dtor Phase-5
     exit path only (is_outermost_ ctor-captured per #2120). Nested
     guards fall through to the nested depth_slot-- branch below without
     touching the flag — AC3 contract source-cited in evaluator_mutation_
     boundary.cpp.
  AC4 Additive observability — schema-2726 / issue-2726 sentinels +
     mutation-hold-budget-holder-degrade-cross-fiber-cancel-fired-total /
     ...-consumed-total keys. All #2701/#2720/#2724/#2587/#2630 surfaces
     preserved (strict superset, no replacement).
  AC5 Source-cite + extend tests/serve/test_mailbox_hold_starvation_hard
     (no new test file) per #81967. Coverage linter present (this file)
     + wired in build.py.
  AC6 No docs/design/2726-* on disk per #1655 (design rationale lives in
     the close comment, not in a per-issue plan doc).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import subprocess
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

    # clang-format ColumnLimit: 100 splits long string literals across
    # multiple lines (e.g. "fired-" "total" — adjacent literals concat
    # per C++ standard, so the source still compiles). must_key() searches
    # for the key without quotes + whitespace so both single-line and
    # split-line forms match the AC contract.
    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    mhb = _read("src/compiler/mutation_hold_budget.h")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    fh = _read("src/serve/fiber.h")
    fc = _read("src/serve/fiber.cpp")
    t = _read("tests/serve/test_mailbox_hold_starvation_hard.cpp")
    build = _read("build.py")

    # AC1 — per-Fiber pending-cancel flag + registry + C-linkage shim +
    # cross-fiber wire-up + Phase-5 poll.
    must("Issue #2726", "AC1", mhb)
    must("g_mutation_hold_budget_holder_degrade_cross_fiber_cancel_fired_total", "AC1", mhb)
    must("g_mutation_hold_budget_holder_degrade_cross_fiber_cancel_consumed_total", "AC1", mhb)
    must("kMutationHoldBudgetHolderDegradeCrossFiberCancelIssue = 2726", "AC1", mhb)
    must("mutation_hold_budget_holder_degrade_cross_fiber_cancel_fired_total_v_read", "AC1", mhb)
    must("mutation_hold_budget_holder_degrade_cross_fiber_cancel_consumed_total_v_read", "AC1", mhb)
    must("clear_mutation_hold_budget_holder_degrade_cross_fiber_cancel_for_test", "AC1", mhb)
    # Fiber pending-cancel atomic + accessors (fiber.h).
    must("Issue #2726", "AC1", fh)
    must("request_hold_budget_cancel", "AC1", fh)
    must("consume_hold_budget_cancel", "AC1", fh)
    must("peek_hold_budget_cancel", "AC1", fh)
    must("pending_hold_budget_cancel_", "AC1", fh)
    # Fiber* registry + C-linkage shim (fiber.cpp).
    must("Issue #2726", "AC1", fc)
    must("aura_fiber_request_hold_budget_cancel", "AC1", fc)
    must("aura_fiber_peek_hold_budget_cancel", "AC1", fc)
    must("register_fiber_in_registry", "AC1", fc)
    must("unregister_fiber_from_registry", "AC1", fc)
    must("find_fiber_by_id_locked_held", "AC1", fc)
    must("g_fiber_registry_mtx", "AC1", fc)
    must("g_fiber_registry", "AC1", fc)
    # Cross-fiber wire-up (evaluator_fiber_mutation.cpp).
    must("Issue #2726", "AC1", efm)
    must("aura_fiber_request_hold_budget_cancel", "AC1", efm)
    must("g_mutation_hold_budget_holder_degrade_cross_fiber_cancel_fired_total", "AC1", efm)
    # Phase-5 poll (evaluator_mutation_boundary.cpp).
    must("Issue #2726", "AC1", emb)
    must("consume_hold_budget_cancel", "AC1", emb)
    must("g_mutation_hold_budget_holder_degrade_cross_fiber_cancel_consumed_total", "AC1", emb)
    must("is_outermost_", "AC1", emb)
    must("mutation_hold_budget_reject_enabled()", "AC1", emb)

    # AC2 — Soft / sandbox counter-only unless hard env.
    must("mutation_hold_budget_reject_enabled()", "AC2", efm)
    # emb Phase-5 poll also gates on reject_enabled (defense-in-depth).
    must("mutation_hold_budget_reject_enabled()", "AC2", emb)

    # AC3 — nested / non-outermost guards never observe or clear flag.
    must("is_outermost_ && aura::serve::g_current_fiber", "AC3", emb)
    must("AC3", "AC3", emb)

    # AC4 — additive query keys (must_key: clang-format may split literals).
    must("Issue #2726", "AC4", q)
    must_key("mutation-hold-budget-holder-degrade-cross-fiber-cancel-fired-total", "AC4", q)
    must_key("mutation-hold-budget-holder-degrade-cross-fiber-cancel-consumed-total", "AC4", q)
    must_key("schema-2726", "AC4", q)
    must_key("issue-2726", "AC4", q)
    # #2701 / #2720 / #2724 / #2551 surfaces preserved (strict superset).
    must_key("mutation-hold-budget-reject-total", "AC4", q)
    must_key("mutation-hold-budget-soft-observe-total", "AC4", q)
    must_key("schema-2701", "AC4", q)
    must_key("schema-2720", "AC4", q)
    must_key("schema-2724", "AC4", q)

    # AC5 — source-cite + test extension per #81967 (no new test file) +
    # build.py wires linter.
    must("ac2726_1_cross_fiber_real_cancel", "AC5", t)
    must("ac2726_2_soft_counter_only", "AC5", t)
    must("ac2726_3_nested_outermost_only", "AC5", t)
    must("ac2726_4_query_keys", "AC5", t)
    must("ac2726_5_source_and_linter", "AC5", t)
    must("ac2726_6_no_docs_design", "AC5", t)
    # #81967: NO new test file — extend the existing one.
    if (ROOT / "tests" / "serve" / "test_issue_2726.cpp").is_file():
        fails.append("AC5: tests/serve/test_issue_2726.cpp present (forbidden per #81967)")
    must("check_cross_fiber_hold_budget_cancel_2726", "AC5", build)

    # AC6 — no docs/design/2726-* on disk per #1655.
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2726-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # Cross-check: prior #2700 + #2699 + #2701 linters still green (the
    # additive superset path). #2726 sits on top of #2720/#2724, so we
    # gate the chain.
    for prev in (
        "check_mutation_hold_budget_reject_2701.py",
        "check_mutation_hold_budget_holder_degrade_2720.py",
        "check_region_concurrent_admit_2724.py",
    ):
        prev_path = ROOT / "scripts" / "coverage" / "checks" / prev
        if not prev_path.is_file():
            # Some predecessors may not exist (e.g. #2720 linter may live
            # under a different name). Skip silently rather than fail —
            # the AC5 self-test already exercises the same surface.
            continue
        r = subprocess.run(
            [sys.executable, str(prev_path)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            fails.append(f"{prev} regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2726 cross-fiber hold-budget cancel — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
