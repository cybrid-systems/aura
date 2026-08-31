#!/usr/bin/env python3
"""Issue #3459: grant_macro_self_evo refuses mid==0 under production.

#3090 made CapabilityRegistry::grant refuse prov.mutation_id == 0 under
Restricted/Strict (grant-mid-refused). grant_macro_self_evo still
synthesized mid = epoch|1 at the front, making that refuse dead, and
security:grant-effect! dual-wrote + returned #t unconditionally — the
Agent control plane reported success on a refused grant.

Contract:
  AC1 production + prov.mutation_id == 0 → SE grant-mid-refused +
      capability_grant_mid_refused_total bump, no by_tenant write, no MSE OR
  AC2 phantom mid synthesis is Soft/Off-only contract (Off mid=1 stays)
  AC3 security:grant-effect! reports refusal (primitive_error), no MSE
      seed on a dropped base grant; grant-capability! hygiene (error when
      the write did not land; dedup-already-held stays #t)
  AC4 grant_effect_capability / grant / grant_locked report landed-ness
      (bool); grant() #3090 refuse path unchanged
  AC5 SSOT callers + SE reasons reused (no new query key, no new counter
      mid-struct)
  AC6 tests/compiler/test_macro_self_evo_capability.cpp extends ac3459_*;
      no docs/design/, no tests/issues/

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

    cm = _read("src/core/capability_model.hh")
    must("Issue #3459", "AC1 stamp", cm)
    must("Soft/Off-only contract", "AC2 synthesis gating", cm)
    must("bool grant_macro_self_evo", "AC1 refuse reporter", cm)
    must("bool grant(", "AC4 grant reports landed", cm)
    must("bool grant_locked(", "AC4 grant_locked reports landed", cm)
    must('"grant-mid-refused"', "AC5 reason reused", cm)

    ixx = _read("src/compiler/evaluator.ixx")
    must("bool grant_effect_capability", "AC4 decl bool", ixx)

    sec = _read("src/compiler/evaluator_security.cpp")
    must("bool Evaluator::grant_effect_capability", "AC4 def bool", sec)
    must("return landed;", "AC4 landed propagated", sec)

    prim = _read("src/compiler/evaluator_primitives_security.cpp")
    must("Issue #3459", "AC3 prim stamp", prim)
    must("security:grant-effect!: grant refused", "AC3 refusal reported", prim)
    must("macro-self-evo grant refused", "AC3 MSE refusal reported", prim)
    must("security:grant-capability!: grant did not land", "AC3 hygiene", prim)

    test = _read("tests/compiler/test_macro_self_evo_capability.cpp")
    must("ac3459_1_production_mid_refuse", "AC6 AC1 test", test)
    must("ac3459_2_live_mid_lands_with_m", "AC6 AC2 test", test)
    must("ac3459_3_no_explicit_ta_denied", "AC6 AC3 test", test)
    must("ac3459_4_off_synthesis_contract", "AC6 AC4 test", test)
    must("ac3459_5_source_cite_no_new_key", "AC6 source-cite test", test)

    build = _read("build.py")
    must("check_grant_mse_mid_refuse_3459", "AC6 build.py wires linter", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3459-*")):
            fails.append(f"AC6: docs/design/{f.name}")
    if (ROOT / "tests" / "issues" / "test_issue_3459.cpp").is_file():
        fails.append("AC6: tests/issues/test_issue_3459.cpp")

    if fails:
        print("FAIL #3459 grant_mse_mid_refuse:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3459 grant_mse_mid_refuse: one refuse policy (production mid==0)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
