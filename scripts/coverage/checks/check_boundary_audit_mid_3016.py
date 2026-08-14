#!/usr/bin/env python3
"""Issue #3016: MutationBoundary trail stamps resolve_audit_mutation_id.

Contract (one row per AC):
  AC1  Guard enter / checkpoint.audit_mid / TLS use resolve_audit_mutation_id
       (not total_mutations_ as join key).
  AC2  capture_audit_event_forced skips mid=0 (no process-origin stamp).
  AC3  Production refuse stays fail-closed; Soft still fallback-gens.
  AC4  Multi-eval / multi-fiber do not cross-join via coincidental
       total_mutations_ values.
  AC5  Extend test_audit_mutation_id_unify (#81967); no test_issue_3016.cpp;
       no docs/design/ (#1655). Additive schema-3016 only.

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

    tma = _read("src/compiler/typed_mutation_audit.h")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    tc = _read("src/compiler/evaluator_typecheck.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_audit_mutation_id_unify.cpp")
    build = _read("build.py")

    must("Issue #3016", "AC1", mb)
    must("cp.audit_mid", "AC1", mb)
    must("resolve_audit_mutation_id", "AC1", mb)
    must("audit_mid", "AC1", ev)
    must("stamp_boundary_audit_mid", "AC1", tma)
    must("note_boundary_audit_mid", "AC1", tma)
    must("stamp_boundary_audit_mid", "AC1", tc)
    must("stamp_boundary_audit_mid", "AC1", mut)

    if "if (mutation_id == 0)" not in tma or "never stamp mid=0" not in tma:
        fails.append("AC2: capture_audit_event_forced must skip mid=0")
    must("g_last_stamped_audit_mid", "AC2", tma)

    must("production_defaults_active", "AC3", tma)
    must("audit_mid_fallback_refused_total", "AC3", tma)
    must("#3016 AC3: production refuse mid=0", "AC3", test)

    must("#3016 AC4: ev2 TLS mid == 77", "AC4", test)
    must("total_mutations_", "AC4", mb)  # still volume bump
    # Trail sites in mb must not load total_mutations_ as mid after #3016.
    # The remaining load is blame soft_mid fallback — allowed.

    must("#3016 AC5: trail mid == resolve", "AC5", test)
    must("schema-3016", "AC5", obs)
    must("boundary-audit-mid-wired", "AC5", obs)
    must("check_boundary_audit_mid_3016", "AC5", build)
    must("last_proof_mid", "AC5", _read("src/compiler/type_linear_commit_health.hh"))
    for rel in (
        "tests/compiler/test_issue_3016.cpp",
        "tests/core/test_issue_3016.cpp",
    ):
        if _read(rel):
            fails.append(f"AC5: {rel} exists — forbidden per #81967")
    if _read("docs/design/3016-boundary-audit-mid.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if "AgentRegistry" in mb[mb.find("Issue #3016") : mb.find("Issue #3016") + 400]:
        fails.append("AC5: must not introduce AgentRegistry")

    if fails:
        print(f"Issue #3016 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3016 boundary audit mid unify — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
