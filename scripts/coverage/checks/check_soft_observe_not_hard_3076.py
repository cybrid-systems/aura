#!/usr/bin/env python3
"""Issue #3076: Soft-observe counters are not Hard production guarantees.

Under production_defaults, a violation face that has a Hard sibling
(restamp-lag / restamp-torn / hygiene-protected / isolation-deny) must
Hard-reject. Soft observe must not increment on that face.

Contract (one row per AC):
  AC1 production_defaults + Hard-sibling face → Hard reject;
      Soft observe stays 0.
  AC2 Soft / sandbox=off → Soft observe only (behavior unchanged).
  AC3 Existing Hard counters (reject / prevented / denied) preserved.
  AC4 Agent stats distinguish Soft vs Hard (schema-3076 /
      soft-observe-not-hard-guarantee).
  AC5 Extend test_hygiene_mutate_closed_loop; no invent test file.
  AC6 This linter flags cited Soft increment sites that lack a Hard
      sibling under production.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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

    audit = _read("src/compiler/typed_mutation_audit.h")
    sec = _read("src/compiler/evaluator_security.cpp")
    prov = _read("src/core/provenance_tracker.hh")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    capm = _read("src/core/capability_model.hh")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    gen = _read("src/compiler/evaluator_primitives_stdlib_review.cpp")
    qmid = _read("src/compiler/evaluator_primitives_query_obs_mid.cpp")
    capq = _read("src/compiler/evaluator_primitives_security.cpp")
    test = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    build = _read("build.py")

    must("kSoftObserveNotHardGuaranteeIssue = 3076", "AC1 stamp", audit)
    must("should_hard_reject_soft_sibling", "AC1 helper", audit)
    must("Issue #3076", "AC1 audit", audit)
    must("should_hard_reject_soft_sibling", "AC1 restamp gate", sec)
    must("record_query_stable_ref_restamp_torn_reject", "AC1 Hard sibling", sec)
    must("ac3076_1_production_soft_observe_stays_zero", "AC1 test", test)

    # Soft increment must not sit inside the Hard branch.
    allow = sec
    idx = allow.find("bool Evaluator::allow_query_stable_ref_export")
    # Issue #3487: allow ORs the multi-worker latch on the already-torn
    # path (one extra line). Keep Hard-before-Soft order; 900 chars no
    # longer covers torn_soft_observe.
    body = allow[idx : idx + 1400] if idx >= 0 else ""
    hard = body.find("should_hard_reject_soft_sibling")
    soft = body.find("record_query_stable_ref_restamp_torn_soft_observe")
    if hard < 0 or soft < 0 or soft < hard:
        fails.append("AC1: Soft observe not after Hard-sibling return")
    hard_ret = body.find("return false", hard if hard >= 0 else 0)
    if hard_ret < 0 or not (hard < hard_ret < soft):
        fails.append("AC1: Hard branch does not return before Soft observe")

    must("ac3076_2_soft_observe_only", "AC2 test", test)
    must("record_query_stable_ref_restamp_torn_soft_observe", "AC2 Soft", sec)

    must("record_query_stable_ref_restamp_lag_prevented", "AC3 lag Hard", sec)
    must("record_query_stable_ref_restamp_torn_reject", "AC3 torn Hard", sec)
    must("reject_structural_macro_hygiene", "AC3 hygiene Hard", mut)
    must("capability_fiber_hard_deny_total", "AC3 isolation Hard", capm)
    must("schema-3037", "AC3 3037 preserved", q)
    must("schema-3000", "AC3 3000 preserved", q)

    must("schema-3076", "AC4 stable-ref-stats", q)
    must("soft-observe-not-hard-guarantee", "AC4 tag", q)
    must("schema-3076", "AC4 generation-stats", gen)
    must("schema-3076", "AC4 children-stable-stats", qmid)
    must("schema-3076", "AC4 capability-effect-stats", capq)
    must("Issue #3076", "AC4 provenance comment", prov)
    must("ac3076_4_schema_and_linter", "AC4 test", test)

    must("check_soft_observe_not_hard_3076", "AC5 build.py", build)
    must("Issue #3076", "AC5 hygiene Hard cite", mut)
    must("Issue #3076", "AC5 isolation Soft cite", capm)

    # AC6: cited Soft increment sites must have a named Hard sibling.
    siblings = {
        "record_query_stable_ref_restamp_lag_soft_observe": ("record_query_stable_ref_restamp_lag_prevented"),
        "record_query_stable_ref_restamp_torn_soft_observe": ("record_query_stable_ref_restamp_torn_reject"),
    }
    for soft_fn, hard_fn in siblings.items():
        if soft_fn not in sec:
            fails.append(f"AC6: Soft increment {soft_fn} missing")
        if hard_fn not in sec:
            fails.append(f"AC6: Hard sibling {hard_fn} missing for {soft_fn}")
    # Isolation Soft (fiber mismatch) vs Hard (fiber_hard_deny).
    if "capability_fiber_mismatch_total" not in capm:
        fails.append("AC6: isolation Soft counter missing")
    if "capability_fiber_hard_deny_total" not in capm:
        fails.append("AC6: isolation Hard sibling missing")
    if re.search(r"record_query_stable_ref_restamp_\w+_soft_observe", sec) and (
        "should_hard_reject_soft_sibling" not in sec
    ):
        fails.append("AC6: restamp Soft increment lacks Hard-sibling helper")

    if (ROOT / "tests" / "compiler" / "test_issue_3076.cpp").is_file():
        fails.append("AC5: test_issue_3076.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3076.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3076.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3076-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3076 Soft-observe not Hard guarantee — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
