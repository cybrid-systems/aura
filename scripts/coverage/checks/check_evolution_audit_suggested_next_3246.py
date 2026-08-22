#!/usr/bin/env python3
"""Issue #3246: evolution-audit-decision additive suggested-next.

Fold already exists (#3114); Agents still OR 5+ queries for next action.
This residual adds a pure suggested-next string+code from already-loaded
commit / posture / densify / playbook / schedule flags. Observe-only:
does not execute playbook / reemit / drain / reload.

Contract (one row per AC):
  AC1  additive suggested-next + suggested-next-code; schema-3246
  AC2  still observe-only (no mutate / reemit / recovery)
  AC3  Soft → soft-observe; production mid=0 → none (no invented Success)
  AC4  synthetic posture/commit/densify/playbook → non-ok; all-green → ok
  AC5  extend test_security_audit_unify; planned keys 48; no invent;
       no docs/design/ (#1655)

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

    header = _read("src/compiler/typed_mutation_audit.h")
    sec = _read("src/compiler/evaluator_primitives_security.cpp")
    unify = _read("tests/compiler/test_security_audit_unify.cpp")
    facade = _read("tests/compiler/test_engine_metrics_facade.cpp")
    build = _read("build.py")

    must("kEvolutionAuditSuggestedNextIssue = 3246", "AC1 stamp", header)
    must("decide_evolution_suggested_next", "AC1 pure helper", header)
    must("suggested-next", "AC1 hash key", sec)
    must("suggested-next-code", "AC1 code key", sec)
    must("schema-3246", "AC1 schema", sec)
    must("soft-observe", "AC3 Soft enum", header)
    must("wait-commit-readiness", "AC4 wait string", header)
    must("Does not execute playbook", "AC2 no executor", sec)
    must("observe-only", "AC2 observe-only", sec)
    must("ac3246_3_soft", "AC3 Soft test", unify)
    must("ac3246_4_ok", "AC4 ok test", unify)
    must("ac3246_4_breach", "AC4 breach test", unify)
    must("kEvolutionAuditDecisionPlannedKeys = 48", "AC5 planned 48", sec)
    must("check_evolution_audit_suggested_next_3246", "AC5 build.py", build)
    must("schema-3246", "AC5 facade", facade)
    if _read("tests/compiler/test_issue_3246.cpp"):
        fails.append("AC5: test_issue_3246.cpp present (forbidden #81967)")
    if _read("docs/design/3246-evolution-suggested-next.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3246 evolution_audit_suggested_next:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3246 evolution_audit_suggested_next: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
