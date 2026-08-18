#!/usr/bin/env python3
"""Issue #3130: P0 IR/JIT Move/Drop fast-path half-green residual.

linear_ir_fastpath_try_skip returns true after
commit_readiness.would_allow_commit dropped to false (abort /
densify-steal / force-rollback). The single pure predicate
linear_move_drop_elision_ok closes the half-green window by also
consulting the live commit_readiness face.

Contract:
  AC1 typed_mutation_audit.h: new `linear_move_drop_elision_ok`
     predicate wraps `linear_ir_fastpath_try_skip` (preserves its
     counter semantics) + adds the readiness gate + production-only
     counter bump on would_allow_commit=false. Soft: zero extra
     counter noise (relaxed load only, no bump).
  AC2 ir_executor_impl.cpp: Move/Drop call site uses
     `linear_move_drop_elision_ok()` (replaces
     `linear_ir_fastpath_try_skip()`).
  AC3 Predicate is production-gated (production_defaults_active
     + AuditStrategy::Full) on the would_allow_commit=false path.
  AC4 tests/compiler/test_occurrence_goal_persist_rehydrate.cpp
     extended with ac3130 source-cite. Existing #3030 / #3032 / #3063
     / #3085 / #3099 ACs preserved.
  AC5 Counter reuse: g_linear_fast_path_elide_blocked_production_total
     (existing from #3006). No new query key middle insertion.
  AC6 No new tests/issues/test_issue_3130.cpp (per #81967).

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

    tma = _read("src/compiler/typed_mutation_audit.h")
    ir = _read("src/compiler/ir_executor_impl.cpp")
    test = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")

    # AC1 — new predicate in typed_mutation_audit.h.
    must("linear_move_drop_elision_ok", "AC1", tma)
    must("Issue #3130", "AC1 cite", tma)
    must("if (!linear_ir_fastpath_try_skip())", "AC1 wraps existing predicate", tma)
    must("commit_readiness(commit_readiness_live_policy())", "AC1 readiness gate", tma)
    must("g_linear_fast_path_elide_blocked_production_total", "AC1 counter reused", tma)

    # AC2 — IR Move/Drop call site uses new predicate.
    must("aura::compiler::typed_audit::linear_move_drop_elision_ok()", "AC2", ir)
    # Verify the new predicate is at the Move/Drop fast-path (not just
    # anywhere in the file). The old linear_ir_fastpath_try_skip() at the
    # Move/Drop site must be replaced.
    move_drop_window = ir[ir.find("op == LinearOpKind::Move") :]
    move_drop_window = (
        move_drop_window[: move_drop_window.find("return true;") + len("return true;")]
        if "return true;" in move_drop_window
        else move_drop_window[:500]
    )
    if "linear_move_drop_elision_ok" not in move_drop_window:
        fails.append("AC2: Move/Drop call site not updated to linear_move_drop_elision_ok")
    if "linear_ir_fastpath_try_skip" in move_drop_window:
        fails.append("AC2: Move/Drop call site still uses old linear_ir_fastpath_try_skip")

    # AC3 — production-gated counter bump.
    pred_pos = tma.find("linear_move_drop_elision_ok()")
    pred_end = tma.find("\n}\n", pred_pos) if pred_pos > 0 else -1
    pred_block = tma[pred_pos:pred_end] if pred_end > pred_pos else ""
    must("production_defaults_active()", "AC3 production gate", pred_block)
    must("get_strategy() == AuditStrategy::Full", "AC3 Full strategy gate", pred_block)
    must("cr.would_allow_commit", "AC3 would_allow_commit check", pred_block)
    must("return false;", "AC3 short-circuit on gate miss", pred_block)

    # AC4 — existing ACs preserved.
    must("ac3032_1_prod_miss_invalidates_fast_path", "AC4 #3032 AC1", test)
    must("ac3032_2_soft_observe_only", "AC4 #3032 AC2", test)
    must("ac3032_3_quiet_zero_cost", "AC4 #3032 AC3", test)
    must("ac3032_4_success_bind", "AC4 #3032 AC4", test)
    must("ac3032_5_schema", "AC4 #3032 AC5", test)
    must("ac3032_6_source_and_linter", "AC4 #3032 AC6", test)
    must("ac3063_1_prod_success_blocks_elide", "AC4 #3063 AC1", test)
    must("ac3063_2_soft_zero_extra", "AC4 #3063 AC2", test)
    must("ac3063_3_schema", "AC4 #3063 AC3", test)
    must("ac3063_4_source_and_linter", "AC4 #3063 AC4", test)
    must("ac3085_1_densify_miss_blocks_elision", "AC4 #3085 AC1", test)
    must("ac3099_1_re_sample_in_try_skip", "AC4 #3099 AC1", test)
    must("ac3099_2_no_new_query_key", "AC4 #3099 AC2", test)
    # The new AC must be present too.
    must("ac3130_linear_move_drop_elision_gates_commit_readiness", "AC4 new AC", test)
    must("Issue #3130", "AC4 new AC cite", test)

    # AC5 — counter reuse (no new query key).
    must("g_linear_fast_path_elide_blocked_production_total", "AC5 existing counter", tma)
    # No new middle metrics key in observability_metrics.h.
    obs = _read("src/compiler/observability_metrics.h")
    if "linear_move_drop_elision_blocked" in obs:
        fails.append("AC5: observability_metrics.h has new key (no new query key allowed)")

    # AC6 — no new tests/issues/test_issue_3130.cpp.
    issue_test = _read("tests/issues/test_issue_3130.cpp")
    if issue_test:
        fails.append("AC6: tests/issues/test_issue_3130.cpp exists (must NOT — src/-aligned suite per #81967)")
    # No docs/design/3130-* (per #1655).
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("limit-doc-3130-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        print("check_linear_move_drop_elision_3130: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_linear_move_drop_elision_3130: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
