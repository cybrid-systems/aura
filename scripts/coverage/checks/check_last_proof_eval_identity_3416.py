#!/usr/bin/env python3
"""Issue #3416: last-proof last-writer across steal × dual-Evaluator.

Process-global last_proof faces + green_bind_gen let eval B's stamp
re-open IR/JIT elision for eval A. Residual of #3380/#3379/#3063.

Contract:
  AC1 Production IR/JIT last-proof unbound unless stamper == TLS eval
  AC2 steal/densify keep #3171 invalidate; green bind eval-scoped
  AC3 live_goal_count vs linear_root_count mismatch → Reject force 16
  AC4 Soft/Off: no extra slot/load when quiet
  AC5 no new query key (fold into last-type-linear-commit-proof /
      evolution snapshot); reuse elide counter
  AC6 extend existing steal/IR suites; linter after #3346; no invent

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

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

    tma = _read("src/compiler/typed_mutation_audit.h")
    qstats = _read("src/compiler/evaluator_primitives_query_type_stats.cpp")
    qref = _read("src/compiler/evaluator_primitives_query_reflect.cpp")
    health = _read("src/compiler/type_linear_commit_health.hh")
    t3305 = _read("tests/compiler/test_ir_typed_entry_proof_authority.cpp")
    t3379 = _read("tests/compiler/test_typed_audit_commit_readiness_live_policy.cpp")
    t2553 = _read("tests/compiler/test_commit_readiness_score.cpp")
    tsteal = _read("tests/compiler/test_post_steal_linear_revalidate.cpp")
    tloop = _read("tests/compiler/test_linear_provenance_steal_gc_closed_loop.cpp")
    tdens = _read("tests/serve/test_steal_densify_linear_type_hard_and.cpp")
    build = _read("build.py")

    must("kLastProofEvalIdentityIssue = 3416", "AC1 stamp", tma)
    must("g_last_proof_stamper_eval", "AC1 gauge", tma)
    must("last_proof_bound_to_current_eval", "AC1 helper", tma)
    pub = tma.find("inline void publish_last_proof_face")
    pub_win = tma[pub : pub + 2200] if pub >= 0 else ""
    must("g_last_proof_stamper_eval.store", "AC1 publish stamper", pub_win)
    must("g_tls_audit_commit_readiness_evaluator", "AC1 TLS stamper", pub_win)

    pred = tma.find("ir_typed_entry_commit_readiness_ok() noexcept")
    pred_win = tma[pred : pred + 4500] if pred >= 0 else ""
    must("last_proof_bound_to_current_eval", "AC1 typed entry", pred_win)
    lfp = tma.find("inline bool linear_fast_path_ok() noexcept")
    lfp_win = tma[lfp : lfp + 3500] if lfp >= 0 else ""
    must("last_proof_bound_to_current_eval", "AC1 fast-path", lfp_win)

    inv = tma.find("invalidate_fast_path_before_steal_densify_restamp() noexcept")
    inv_win = tma[inv : inv + 900] if inv >= 0 else ""
    must("g_last_proof_stamper_eval.store(0", "AC2 steal clear stamper", inv_win)
    must("g_rehydrate_miss_invalidate_gen.fetch_add", "AC2 keep #3171 invalidate", inv_win)

    must("reject_stamper_live_goal_linear_root_mismatch", "AC3 helper", tma)
    ac3 = tma.find("inline void reject_stamper_live_goal_linear_root_mismatch")
    ac3_win = tma[ac3 : ac3 + 1200] if ac3 >= 0 else ""
    must("force_reason_code = 16", "AC3 force_reason 16", ac3_win)
    must("kTypeLinearProofOutcomeReject", "AC3 Reject", ac3_win)
    must("live_goal_count == p.linear_root_count", "AC3 count compare", ac3_win)

    bound = tma.find("last_proof_bound_to_current_eval() noexcept")
    bound_win = tma[bound : bound + 800] if bound >= 0 else ""
    must(
        "if (!(production_defaults_active() || get_strategy() == AuditStrategy::Full))",
        "AC4 Soft skip consult",
        bound_win,
    )
    if "else if (hard)" not in pub_win and "if (hard)" not in pub_win:
        fails.append("AC4: publish stamper gated on production/Full")

    must("type-linear-commit-proof-stamper-bound", "AC5 fold last-proof query", qstats)
    must("last-proof-stamper-bound", "AC5 fold evolution snapshot", qref)
    must("last_proof_stamper_bound", "AC5 snapshot field", health)
    if "schema-3416" in qstats or "schema-3416" in qref or "schema-3416" in tma:
        fails.append("AC5: new schema-3416 query key (forbidden)")
    if "g_3416_" in tma:
        fails.append("AC5: new g_3416_* counter (forbidden)")
    must("g_linear_fast_path_elide_blocked_production_total", "AC5 reuse elide", pred_win)

    must("3416", "AC6 3305 suite", t3305)
    must("3416", "AC6 3379 suite", t3379)
    must("3416", "AC6 2553 suite", t2553)
    must("3416", "AC6 post-steal", tsteal)
    must("3416", "AC6 steal-gc", tloop)
    must("3416", "AC6 steal-densify", tdens)
    must("check_last_proof_eval_identity_3416", "AC6 build", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3416.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_3416.cpp present (forbidden)")
    if (ROOT / "tests" / "issues" / "test_issue_3416.cpp").is_file():
        fails.append("AC6: tests/issues/test_issue_3416.cpp present (forbidden)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3416-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("check_last_proof_eval_identity_3416: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3416 last-proof eval identity — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
