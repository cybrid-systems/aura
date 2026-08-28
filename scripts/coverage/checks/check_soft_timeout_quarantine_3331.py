#!/usr/bin/env python3
"""Issue #3331: Soft TIMEOUT + allow_timeout_commit quarantines residual roots.

Residual of #3169/#3081/#3203: Soft + SolverBudget.allow_timeout_commit
keeps TIMEOUT (never SOLVED, never query:type authority) but must not
leave occurrence_priority / pending_full_solve / touched / let-poly
roots Agent-observable as live work. Production hard clear stays #3169.

Contract (one row per AC):
  AC1 Soft + allow_timeout_commit + TIMEOUT → next empty solve_delta SOLVED /
      zero priority roots
  AC2 TIMEOUT + unresolved export; last_type_export_authoritative false
  AC3 Production path unchanged (clear_partial_goals_and_unresolved)
  AC4 Soft locality observe (#2994) out of scope
  AC5 Extend test_solve_delta_unresolved_export; this linter after #3169;
      no invent / no docs/design

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

    ixx = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    obs = _read("src/compiler/observability_metrics.h")
    aud = _read("src/compiler/typed_mutation_audit.h")
    t = _read("tests/compiler/test_solve_delta_unresolved_export.cpp")
    lint3169 = _read("scripts/coverage/checks/check_solve_delta_partial_cleared_3169.py")
    build = _read("build.py")

    must("kSoftTimeoutQuarantineIssue = 3331", "AC1 stamp", ixx)
    must("soft_quarantine_partial_goals_after_timeout", "AC1 helper decl", ixx)
    must("void ConstraintSystem::soft_quarantine_partial_goals_after_timeout() noexcept", "AC1 helper impl", impl)
    must("soft_quarantine_partial_goals_after_timeout();", "AC1 call site", impl)
    must("ac3331_1_soft_timeout_quarantines_roots", "AC1 test", t)

    helper_pos = impl.find("void ConstraintSystem::soft_quarantine_partial_goals_after_timeout() noexcept")
    helper = impl[helper_pos : helper_pos + 1600] if helper_pos >= 0 else ""
    must("touched_roots_.clear()", "AC1 touched", helper)
    must("pending_full_solve_roots_.clear()", "AC1 pending", helper)
    must("occurrence_priority_roots_.clear()", "AC1 occurrence", helper)
    must("let_poly_dirty_roots_.clear()", "AC1 let-poly", helper)
    must("dirty_count_ = 0", "AC1 dirty", helper)
    if "occurrence_persist_log_" in helper:
        fails.append("AC1: helper must not touch occurrence_persist_log_")
    must("solve_delta_soft_timeout_quarantine_total", "AC1 counter bump", helper)

    must("last_type_export_authoritative_ = false", "AC2 #3081 authority", impl)
    must("return SolveResult::TIMEOUT", "AC2 keep TIMEOUT", impl)
    must("ac3331_2_timeout_unresolved_not_authoritative", "AC2 test", t)
    must("kSoftTimeoutExportNonAuthoritativeIssue = 3081", "AC2 #3081 stamp", ixx)

    must("void ConstraintSystem::clear_partial_goals_and_unresolved() noexcept", "AC3 #3169 helper", impl)
    must("if (!aura::compiler::typed_audit::production_defaults_active())", "AC3 #3169 production gate", impl)
    must("ac3331_3_production_unchanged", "AC3 test", t)
    esc_pos = impl.find("SolveResult ConstraintSystem::escalate_if_production")
    loc_fn = impl.find("ConstraintSystem::escalate_locality_slo_if_production")
    esc = impl[esc_pos:loc_fn] if esc_pos >= 0 and loc_fn > esc_pos else impl
    if "if (!prod && budget.allow_timeout_commit)" not in esc:
        fails.append("AC3: quarantine must stay on Soft allow_timeout_commit path")
    if "clear_partial_goals_and_unresolved();" not in esc:
        fails.append("AC3: production still calls clear_partial_goals_and_unresolved")
    if "soft_quarantine_partial_goals_after_timeout();" not in esc:
        fails.append("AC3: Soft allow_timeout_commit must call quarantine helper")

    loc_pos = impl.find("ConstraintSystem::escalate_locality_slo_if_production")
    loc = impl[loc_pos : loc_pos + 2800] if loc_pos >= 0 else ""
    if "soft_quarantine_partial_goals_after_timeout" in loc:
        fails.append("AC4: locality SLO must not call Soft TIMEOUT quarantine")
    must("ac3331_4_locality_observe_out_of_scope", "AC4 test", t)

    must("solve_delta_soft_timeout_quarantine_total{0}", "AC5 metrics", obs)
    must("solve_delta_soft_timeout_quarantine_total{0}", "AC5 audit", aud)
    must("Issue #3331", "AC5 metrics cite", obs)
    must("check_soft_timeout_quarantine_3331", "AC5 build.py", build)
    must("ac3331_5_source_and_linter", "AC5 test", t)
    must("Issue #3169", "AC5 3169 linter retained", lint3169)
    prev = build.find("check_solve_delta_partial_cleared_3169")
    ours = build.find("check_soft_timeout_quarantine_3331")
    if ours < 0:
        fails.append("AC5: linter must be wired in build.py")
    elif prev >= 0 and ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3169")

    if "g_3331_" in impl or "g_3331_" in ixx:
        fails.append("AC5: new g_3331_* counter")
    if (ROOT / "tests" / "compiler" / "test_issue_3331.cpp").is_file():
        fails.append("AC5: test_issue_3331.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3331-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3331 Soft TIMEOUT quarantine — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
