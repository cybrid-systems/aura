#!/usr/bin/env python3
"""Issue #3319: Sampled deny path still emits SE under production_defaults_active.

Under AuditStrategy::Sampled + production_defaults_active(), a
non-contextual deny (hygiene / hard-gate / AOT fail) could skip
SecurityEvent emission when should_audit() was false. Agent then cannot
reconstruct a complete evidence chain for that mid from query:security-audit.

Contract:
  AC1 production + Sampled + non-contextual deny → SE with non-zero mid
      (or mid=0 + mid-fallback-refused) and stable reason
  AC2 Soft/Off → no extra SE
  AC3 Full trail + SE unchanged
  AC4 SE.mutation_id == trail.mutation_id when trail written
  AC5 restore-first deny stamp
  AC6 no new query key (reuse query:security-audit + mid filter)
  AC7 counters append-only if added; reuse emit_invariant_deny_se

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

    typed = _read("src/compiler/typed_mutation_audit.h")
    tc = _read("src/compiler/evaluator_typecheck.cpp")
    bound = _read("src/compiler/evaluator_mutation_boundary.cpp")
    test = _read("tests/compiler/test_security_audit_unify.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_security.cpp") + _read(
        "src/compiler/evaluator_primitives_query_type_stats.cpp"
    )

    must("kSampledDenySeEmitIssue = 3319", "AC1 stamp", typed)
    must("emit_invariant_deny_se", "AC1 helper", typed)
    must("Issue #3319", "AC1 typed cite", typed)
    must("production_defaults_active()", "AC1 prod gate", typed)
    must("capture_macro_hygiene_audit", "AC1 hygiene", typed)
    must('name, "hygiene"', "AC1 hygiene kind", typed)
    must("emit_invariant_deny_se", "AC1 hard-gate", tc)
    must("aot-hotupdate", "AC1 aot deny SE", typed)
    must("#3319 AC1", "AC1 test", test)

    must("apply_dev_audit_defaults", "AC2 soft", test)
    must("#3319 AC2", "AC2 test", test)

    must("AuditStrategy::Full", "AC3 Full", test)
    must("#3319 AC3", "AC3 test", test)

    must("e.mutation_id == te.mutation_id", "AC4 join", test)
    must("query:security-audit", "AC4 query", test)

    must("record_boundary_deny_after_restore", "AC5 boundary", bound)
    must("dual_clear_coercion_state_on_abort", "AC5 restore", bound)
    must("#3319 AC5", "AC5 test", test)

    must("check_sampled_deny_se_emit_3319", "AC6/AC7 build.py", build)
    must("#3319 AC6", "AC6 test", test)
    if "schema-3319" in q:
        fails.append("AC6: new schema-3319 query key")
    if "g_3319_" in typed or "g_3319_" in tc or "g_3319_" in bound:
        fails.append("AC7: new g_3319_* counter")
    if (ROOT / "tests" / "compiler" / "test_issue_3319.cpp").is_file():
        fails.append("AC7: tests/compiler/test_issue_3319.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3319.cpp").is_file():
        fails.append("AC7: tests/issues/test_issue_3319.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3319-*")):
            fails.append(f"AC7: docs/design/{f.name}")

    if fails:
        print("FAIL #3319 sampled_deny_se_emit:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3319 sampled_deny_se_emit: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
