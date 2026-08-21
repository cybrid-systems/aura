#!/usr/bin/env python3
"""Issue #3226: production soundness sample must run real partial≡full compare.

#2245 sampled incremental soundness but trivially bumped prod_ok
(partial vs partial). Production sample hit now full-lowers the same
lambda and compares via #2113 ir_function_equivalent. Mismatch
force-fulls. Soft / sample_bp==0 skip lower. No new query keys.

Contract:
  AC1 Production sample hit → real full-lower + IR equivalence
  AC2 Injected IR divergence → mismatch_prod + force-full
  AC3 Healthy path increments prod_ok only after compare succeeds
  AC4 sample_bp==0 / Soft never-sampled: zero extra lower
  AC5 No new query keys; extend existing soundness stats only
  AC6 Extend oracle suite + linter; no invent / docs/design

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

    dirty = _read("src/compiler/service_dirty.cpp")
    t = _read("tests/compiler/test_incremental_soundness_oracle.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    old = _read("scripts/coverage/checks/check_incremental_soundness_prod_coverage.py")

    must("Issue #3226", "AC1 cite", dirty)
    must("lower_full_same_lambda", "AC1 helper", dirty)
    must("lower_function_at", "AC1 full lower", dirty)
    must("ir_function_equivalent", "AC1 #2113 compare", dirty)
    must("ir_module_equivalent", "AC1 module alias", dirty)
    if "trivially pass" in dirty:
        fails.append("AC1: trivial prod_ok pass still present")

    must("inject_soundness_under_dirty_for_test", "AC2 inject", dirty)
    must("incremental_soundness_mismatch_prod_total", "AC2 mismatch", dirty)
    must("mark_all_blocks_dirty", "AC2 force-full", dirty)
    must("RelowerFallbackReason::Other", "AC2 fallback", dirty)
    must("ac11_prod_inject_mismatch_forces_full", "AC2 test", t)

    must("incremental_soundness_prod_ok_total", "AC3 ok after compare", dirty)
    must("ac10_prod_sample_real_compare", "AC3 test", t)

    must("if (sample_eff_bp > 0)", "AC4 bp gate", dirty)
    must("ac12_sample_bp_zero_no_full_lower", "AC4 test", t)

    must("lower_full_same_lambda", "AC5 #2245 linter updated", old)
    if "query:incremental-soundness-prod-compare" in q:
        fails.append("AC5: new query key")
    if "g_3226_" in dirty:
        fails.append("AC5: new g_3226_* counter")

    must("check_prod_soundness_real_compare_3226", "AC6 build.py", build)
    if (ROOT / "tests" / "issues" / "test_issue_3226.cpp").is_file():
        fails.append("AC6: forbidden tests/issues/test_issue_3226.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3226.cpp").is_file():
        fails.append("AC6: forbidden tests/compiler/test_issue_3226.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3226-*")):
            fails.append(f"AC6: docs/design/{f.name}")

    if fails:
        print("FAIL #3226 prod_soundness_real_compare:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3226 prod_soundness_real_compare: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
