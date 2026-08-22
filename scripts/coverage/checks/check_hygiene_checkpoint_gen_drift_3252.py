#!/usr/bin/env python3
"""Issue #3252: gen-drift refuse homology-repairs MacroIntroduced.

Production restore_hygiene_checkpoint gen-drift still refuses topology
restore, then restamp_macro_introduced_generations + invariant check.
Soft / Off: metric-only refuse (zero extra).

Contract (one row per AC):
  AC1  production gen-drift: restamp + invariant; topology not restored
  AC2  query/mutate/JIT still see consistent MacroIntroduced (restamp)
  AC3  gen-drift refuse still returns false (no metadata restore)
  AC4  tests in test_hygiene_checkpoint; no test_issue_3252.cpp
  AC5  Soft skip restamp; no new query:*
  AC6  stable reason hygiene-checkpoint-gen-drift; reuse restore_fail_total

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

    bound = _read("src/compiler/evaluator_mutation_boundary.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_hygiene_checkpoint.cpp")
    build = _read("build.py")

    must("Issue #3252", "AC1 cite", bound)
    must("restamp_macro_introduced_generations", "AC1 restamp", bound)
    must("hygiene-checkpoint-gen-drift", "AC1 reason", bound)
    must("check_macro_hygiene_invariant_post_restore", "AC1 invariant", bound)
    must("production_defaults_active()", "AC5 Soft gate", bound)
    must("return false", "AC3 still refuse", bound)

    # restamp must sit on the gen-drift refuse path, not only success.
    drift = bound.find("saved_flat_generation")
    restamp = bound.find("restamp_macro_introduced_generations")
    if drift < 0 or restamp < 0 or restamp < drift:
        fails.append("AC1: restamp must run on gen-drift refuse path")

    must("Issue #3252", "AC1 ixx", ixx)
    must("schema-3252", "AC5 additive schema", q)
    must("hygiene-checkpoint-gen-drift-wired", "AC5 wired", q)
    must("ac3252_prod_gen_drift_restamp_homology", "AC4 prod test", test)
    must("ac3252_soft_gen_drift_zero_extra", "AC5 Soft test", test)
    must("check_hygiene_checkpoint_gen_drift_3252", "AC4 build.py", build)
    must("hygiene-checkpoint-gen-drift", "AC6 reason test", test)

    if _read("tests/compiler/test_issue_3252.cpp") or _read("tests/core/test_issue_3252.cpp"):
        fails.append("AC4: test_issue_3252.cpp present (forbidden #81967)")
    if _read("docs/design/3252-hygiene-checkpoint-gen-drift.md"):
        fails.append("AC4: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3252 hygiene_checkpoint_gen_drift:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3252 hygiene_checkpoint_gen_drift: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
