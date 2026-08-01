#!/usr/bin/env python3
"""Issue #2434: hard HotPassDodCompliant for all production pipeline stages.

Contract:
  AC1 all pack stages HotPassDodCompliant (static_assert / inventory)
  AC2 concept_rejection stays 0 under production SoA packs
  AC3 pure_wrap_total still advances
  AC4 dirty short-circuit + static_assert pack
  AC5 schema-2434 + test/gate

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    # Issue #2524: pass_manager is a facade; bodies live in pipeline core + impls.
    pm = (
        _read("src/compiler/pass_pipeline_core.ixx")
        + _read("src/compiler/pass_impls.ixx")
        + _read("src/compiler/pass_manager.ixx")
    )
    cc = _read("src/core/concept_constraints.ixx")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_hot_pass_hard_dod_2434.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1
    must("Pipeline stage must be HotPassDodCompliant", "AC1", pm)
    must("check_production_pipeline_packs_2434", "AC1", pm)
    must("HotPassDodCompliant<DCEPass>", "AC1", pm)
    must("HotPassDodCompliant<InlinePass>", "AC1", pm)
    must("HotPassDodCompliant<TCOPass>", "AC1", pm)
    must("HotPassDodCompliant<MonomorphizePass>", "AC1", pm)
    must("HotPassDodCompliant<LinearOwnershipPass>", "AC1", pm)
    must("2434 AC1", "AC1", test)

    # AC2
    must("pass_pipeline_concept_rejection_total", "AC2", pm)
    must("LegacyPass<T>", "AC2", pm)
    must("2434 AC2", "AC2", test)

    # AC3
    must("pass_pipeline_pure_wrap_total", "AC3", pm)
    must("kPureWrap = true", "AC3", pm)
    must("2434 AC3", "AC3", test)

    # AC4
    must("check_pipeline_dod_compliance", "AC4", pm)
    must("2434 AC4", "AC4", test)

    # AC5
    must("Issue #2434", "AC5", pm)
    must("Issue #2434", "AC5", cc)
    must("schema-2434", "AC5", q)
    must("pass_pipeline_hard_dod_wired", "AC5", pm)
    must("2434 AC5", "AC5", test)
    must("check_hot_pass_hard_dod_2434", "gate", build)
    must("cmd_hot_pass_hard_dod_coverage", "gate", build)
    must("test_hot_pass_hard_dod_2434", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: hot pass hard dod #2434 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
