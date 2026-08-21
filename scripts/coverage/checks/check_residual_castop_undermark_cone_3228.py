#!/usr/bin/env python3
"""Issue #3228: residual CastOp + under-mark must force type∪IR cone.

After DeadCoercion / type-txn wipe, an empty cone + residual persist
(columnar under-mark) skipped remirror. Production always remirrors
persist into the cone and treats empty-cone residual as dirty before
commit_readiness. Soft / empty persist: 0 extra. Reuses #3065/#3120.

Contract:
  AC1 Production residual + empty cone → remirror + re-typecheck seed
  AC2 Soft observe; quiet empty persist
  AC3 No regression on #3065 / #3120
  AC4 Extend dead_coercion_* + dirty_cascade + incremental_type; linter

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

    dirty = _read("src/compiler/dirty_propagation.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    etc = _read("src/compiler/evaluator_typecheck.cpp")
    passes = _read("src/compiler/pass_impls.ixx")
    t = _read("tests/compiler/test_dead_coercion_dirty_cone.cpp")
    col = _read("tests/compiler/test_dead_coercion_columnar.cpp")
    batch = _read("tests/compiler/test_batch_dirty_cascade.cpp")
    inc = _read("tests/compiler/test_incremental_type_batch.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_query.cpp") + _read(
        "src/compiler/evaluator_primitives_query_type_stats.cpp"
    )

    must("kResidualCastopUndermarkConeIssue", "AC1 stamp", dirty)
    must("force_residual_castop_undermark_into_cone", "AC1 helper", dirty)
    must("Issue #3120 / #3228", "AC1 type txn", impl)
    must("force_residual_castop_undermark_into_cone", "AC1 empty affected", impl)
    must("force_residual_castop_undermark_into_cone", "AC1 mutate/commit", etc)
    must("Issue #3228", "AC1 columnar leftover", passes)
    must("ac3228_1_empty_cone_remirrors_residual", "AC1 test", t)
    if "if (!cone.empty())" in impl and "remirror_persisted_residual_castops" in impl:
        # The old gate must not wrap remirror.
        idx = impl.find("(void)dirty::remirror_persisted_residual_castops();")
        if idx < 0:
            fails.append("AC1: remirror call missing")
        else:
            window = impl[max(0, idx - 80) : idx]
            if "if (!cone.empty())" in window:
                fails.append("AC1: remirror still gated on !cone.empty()")

    must("ac3228_2_soft_quiet", "AC2 test", t)
    must("if (!residual_castop_persist_active())", "AC2 persist gate", dirty)

    must("force_dead_coercion_elim_into_cone", "AC3 #3065", dirty)
    must("remirror_persisted_residual_castops", "AC3 #3120", dirty)
    must("ac3228_3_no_regression_3065_3120", "AC3 test", t)
    if "schema-3228" in q:
        fails.append("AC3: new schema-3228 query key")
    if "g_3228_" in dirty:
        fails.append("AC3: new g_3228_* counter")

    must("check_residual_castop_undermark_cone_3228", "AC4 build.py", build)
    must("3228", "AC4 columnar", col)
    must("3228", "AC4 cascade", batch)
    must("3228", "AC4 incremental_type", inc)
    if (ROOT / "tests" / "issues" / "test_issue_3228.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3228.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3228.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3228.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3228-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3228 residual_castop_undermark_cone:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3228 residual_castop_undermark_cone: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
