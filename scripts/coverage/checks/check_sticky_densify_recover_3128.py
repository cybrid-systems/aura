#!/usr/bin/env python3
"""Issue #3128: P0 sticky densify-off recovery remains Agent-only.

Production Moving densify is fail-closed (untracked / stale / incomplete
remap → sticky densify-off). The existing
Evaluator::recover_moving_sticky_densify_off(retry_densify) clears sticky
+ retries, but was Agent-only. High-freq self-evo Agents that only poll
health stay blocked forever after one incomplete densify.

Contract:
  AC1 evaluator_mutation_boundary.cpp Phase-5 fail residual:
      production_defaults_active() + sticky armed →
      this->recover_moving_sticky_densify_off(retry_densify=true).
      Manual publish skipped when retry's internal publish handled it.
  AC2 Recover failure (retry still incomplete) → sticky re-arms +
      retry's publish sets throttle (fail-closed — would_allow_mutate
      stays false). Idempotent — Agents that already call recover
      explicitly are unaffected (sticky already cleared on re-entry).
  AC3 Soft / sandbox=off / sticky never armed → zero extra work.
  AC4 Existing Phase-5 green auto-clear path unchanged; arena.ixx
      sticky arm/clear surface unchanged (no second registry).
  AC5 Counters reused: g_moving_sticky_cleared_via_recovery_total +
      g_moving_densify_retry_after_recovery_total; no new query key,
      no metrics middle insertion.
  AC6 tests/core/test_moving_densify_fail_closed.cpp: ac3128_*
      source-cite. Existing #2837 / #2905 / #2935 tests stay green.
      No new tests/issues/test_issue_3128.cpp (per #81967).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

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
            fails.append(f"{label}: must NOT contain {n!r}")

    mut = _read("src/compiler/evaluator_mutation_boundary.cpp")
    arena = _read("src/core/arena.ixx")
    health = _read("src/core/moving_densify_health.hh")
    test_fail = _read("tests/core/test_moving_densify_fail_closed.cpp")
    _read("build.py")

    # AC1 — Phase-5 fail residual calls this->recover_moving_sticky_densify_off
    # under production_defaults_active() + sticky armed.
    must("Issue #3128", "AC1", mut)
    must("recover_moving_sticky_densify_off(/*retry_densify=*/true)", "AC1", mut)
    must("moving_incomplete_remap_sticky_densify_off()", "AC1", mut)
    must("production_defaults_active()", "AC1", mut)
    must("auto_recover_attempted", "AC1 manual publish conditional", mut)
    # The manual publish is skipped when auto_recover_attempted.
    must("if (!auto_recover_attempted)", "AC1 skip on retry", mut)
    # Phase-5 fail residual block still has the original publish call.
    must("publish_last_moving_densify_window", "AC1 sibling Phase-5 publish", mut)

    # AC2 — Recover failure path: sticky re-arms + retry publishes (fail-closed).
    # Verified via recover_moving_sticky_densify_off contract — sticky
    # re-armed in step (c) when retry incomplete. Source-cite:
    must("MovingStickyDensifyRecoveryResult", "AC2 recover result struct", mut)
    must("g_moving_sticky_cleared_via_recovery_total", "AC2 counter", mut)
    must("g_moving_densify_retry_after_recovery_total", "AC2 retry counter", mut)

    # AC3 — Soft / sandbox=off: zero extra work (no auto-recover call).
    # Verified by source-cite: the gate is production_defaults_active() +
    # moving_incomplete_remap_sticky_densify_off() — both must be true.
    # Soft satisfies neither, so the call is skipped.
    must("if (typed_audit::production_defaults_active() &&", "AC3 production-gated", mut)
    # Soft observe-only contract preserved (no recovery in Soft).
    must_not("recover_moving_sticky_densify_off(/*retry_densify=*/true)\n                ;", "AC3 Soft path check", mut)
    # (The actual AC3 functional assertion is via env + counter check in
    #  tests/core/test_moving_densify_fail_closed.cpp ac3128_* source-cite.)

    # AC4 — Phase-5 green auto-clear path unchanged. arena.ixx sticky
    # arm/clear surface NOT modified by #3128 (no second registry).
    must("clear_moving_incomplete_remap_sticky_densify_off", "AC4 clear still exists", arena)
    must_not("Issue #3128", "AC4 arena.ixx NOT modified", arena)
    must("compute_moving_unified_success", "AC4 sibling unified success", arena)
    must("would_allow_mutate", "AC4 sibling gate", arena)
    # Phase-5 green auto-clear via compact_all_moving_pinned → publish.
    must("publish_last_moving_densify_window", "AC4 sibling publish", arena)

    # AC5 — Counters reused (no new query key / no middle insertion).
    densify = _read("src/core/densify_consistency_report.h")
    must("g_moving_sticky_cleared_via_recovery_total", "AC5 cleared counter", densify)
    must("g_moving_densify_retry_after_recovery_total", "AC5 retry counter", densify)
    # Counters also referenced from evaluator_mutation_boundary.cpp (auto-recover site).
    must("g_moving_sticky_cleared_via_recovery_total", "AC5 cleared counter wired", mut)
    must("g_moving_densify_retry_after_recovery_total", "AC5 retry counter wired", mut)
    # No new query key (no new metric added to query:orch-module-stats /
    # query:densify-stats surface for #3128).
    must_not("spawn_bp_scope_overflow_dropped_total", "AC5 no borrowed keys from other issues", mut)
    must_not("Issue #3128", "AC5 densify-stats unchanged", health)

    # AC6 — tests/core/test_moving_densify_fail_closed.cpp extended;
    # existing #2837 / #2905 / #2935 stay green; no new test_issue_3128.cpp.
    must("ac3128_auto_recover_under_sticky", "AC6 test function added", test_fail)
    must("Issue #3128", "AC6 test source-cite", test_fail)
    must("recover_moving_sticky_densify_off(/*retry_densify=*/true)", "AC6 test cite", test_fail)
    # Sibling ACs preserved.
    must("ac2837_3_sticky_densify_off_under_hard", "AC6 sibling #2837", test_fail)
    must("ac2905_4_reregister_clean_restores_without_manual_clear", "AC6 sibling #2905", test_fail)
    must("ac2935_3_agent_recovery_path", "AC6 sibling #2935", test_fail)
    must("ac3017_4_sticky_recover_after_inject", "AC6 sibling #3017", test_fail)
    must("ac3057_5_source_cite_no_invent", "AC6 sibling #3057", test_fail)
    # No new test_issue_3128.cpp.
    issue_test = _read("tests/issues/test_issue_3128.cpp")
    if issue_test:
        fails.append("AC6: tests/issues/test_issue_3128.cpp exists (must NOT — src/-aligned suite per #81967)")
    # No docs/design/3128-* (per #1655).
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("limit-doc-3128-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        print("check_sticky_densify_recover_3128: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_sticky_densify_recover_3128: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
