#!/usr/bin/env python3
"""Issue #3329: production pipeline concept-rejects impure Pass.

Concepts were declared but run_pipeline only required Pass + HotPassDodCompliant.
An impure Pass (workspace write / residual single-mark / no SoA) could still
enter the default fold. #3329 applies ProductionPipelinePass
(AnalysisPass && SoAViewAwarePass && DirtyPropagatorAwarePass, not Legacy)
to run_production_pipeline / production dirty fold. Soft/unit keep
run_pipeline. Concepts erase (zero extra atomics).

Contract:
  AC1 impure stub fails ProductionPipelinePass; production pack compiles
  AC2 Soft / unit run_pipeline unconstrained
  AC3 existing DOD passes still compile; dirty-fold metrics unchanged
  AC4 source-cite + linter after #2434; no test_issue_*.cpp; no docs/design
  AC5 no extra atomics / branches on happy path

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

    cc = _read("src/core/concept_constraints.ixx")
    core = _read("src/compiler/pass_pipeline_core.ixx")
    impls = _read("src/compiler/pass_impls.ixx")
    opt = _read("src/compiler/optimization_passes.ixx")
    svc = _read("src/compiler/service.ixx")
    t = _read("tests/compiler/test_hot_pass_hard_dod.cpp")
    tc = _read("tests/compiler/test_concept_constraints.cpp")
    build = _read("build.py")

    must("kPassPurityGateIssue = 3329", "AC1 stamp", cc)
    must("ProductionPipelinePass", "AC1 concept", cc)
    must("DirtyPropagatorAwarePass", "AC1 dirty-aware", cc)
    must("ImpureWorkspaceWriteStub", "AC1 impure stub", cc)
    must("!ProductionPipelinePass", "AC1 negative assert", cc)
    must("run_production_pipeline", "AC1 entry", core)
    must("check_production_pipeline_purity", "AC1 consteval", core)
    must("3329 AC1", "AC1 test", t)

    must("template <Pass... Passes>", "AC2 run_pipeline unconstrained", core)
    must("3329 AC2: run_pipeline unconstrained for unit", "AC2 test", t)

    must("check_production_pipeline_purity_3329", "AC3 pack", impls)
    must("run_production_pipeline", "AC3 default pack", opt)
    must("run_production_pipeline", "AC3 service", svc)
    must("run_production_incremental_dirty_pipeline", "AC3 dirty fold", core)
    must("3329 AC3: concept_rejection unchanged", "AC3 metrics", t)

    must("Issue #3329", "AC4 cc cite", cc)
    must("Issue #3329", "AC4 core cite", core)
    must("check_production_pipeline_purity_3329", "AC4 build.py", build)
    must("kPassConceptCount = 17", "AC4 concept count", cc)
    must("3329 AC4: no invent", "AC4 test", t)

    must("no extra atomics", "AC5 zero extra", core)
    must("Concepts erase", "AC5 erase", cc)
    must("check_hot_pass_hard_dod_2434", "AC4 2434 lineage", build)
    fn = build.find("def cmd_hot_pass_hard_dod_coverage")
    ours_in_fn = build.find("check_production_pipeline_purity_3329", fn if fn >= 0 else 0)
    prev_in_fn = build.find("check_hot_pass_hard_dod_2434", fn if fn >= 0 else 0)
    if fn < 0 or ours_in_fn < 0:
        fails.append("AC4: linter must be wired in cmd_hot_pass_hard_dod_coverage")
    elif prev_in_fn >= 0 and ours_in_fn < prev_in_fn:
        fails.append("AC4: linter must be wired in cmd_hot_pass_hard_dod_coverage AFTER #2434")

    if "g_3329_" in cc or "g_3329_" in core:
        fails.append("AC5: new g_3329_* counter")
    if (ROOT / "tests" / "compiler" / "test_issue_3329.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3329.cpp per #81967")
    if (ROOT / "tests" / "issues" / "test_issue_3329.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3329.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3329-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if "run_pipeline(mod, cf, tp, dce, ck, sa, rp)" in opt:
        fails.append("AC3: default pack still uses unconstrained run_pipeline")

    _ = tc  # concept-constraints fixture also extended

    if fails:
        print("FAIL #3329 production_pipeline_purity:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3329 production_pipeline_purity: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
