#!/usr/bin/env python3
"""Issue #2907: Sunset SoAtoAoSBridgePass — force hot DirtyAware onto run_dirty.

Contract:
  AC1 Production packs contain zero SoAtoAoSBridgePass; kTestOnlyAosBridge
  AC2 Hot DirtyAware + SoA stages implement run_dirty / DirtySoAEntry
  AC3 pass_pipeline_concept_rejection path + production_pack_zero_aos_bridge
  AC4 run_production_soa_dirty_hot_pack + service wire; residual bridge free
  AC5 schema-2907 query keys; no docs/design/*; extend existing suite

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

    impls = _read("src/compiler/pass_impls.ixx")
    core = _read("src/compiler/pass_pipeline_core.ixx")
    svc = _read("src/compiler/service.ixx")
    cc = _read("src/core/concept_constraints.ixx")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_soa_dirty_aware_pipeline.cpp")
    build = _read("build.py")

    # AC1
    must("#2907", "AC1", impls)
    must("kTestOnlyAosBridge", "AC1", impls)
    must("kAosBridgeSunsetIssue", "AC1", impls)
    must("check_production_soa_dirty_pack_2907", "AC1", impls)
    must("production_pack_zero_aos_bridge_wired", "AC1", core)
    if "check_pipeline_dod_compliance<SoAtoAoSBridgePass" in impls:
        fails.append("AC1: SoAtoAoSBridgePass appears in production pack inventory")

    # AC2
    must("run_production_soa_dirty_hot_pack", "AC2", impls)
    must("SoaDirtyAwarePass<ConstantFoldingWrap>", "AC2", impls)
    must("SoaDirtyAwarePass<TypePropagationPass>", "AC2", impls)
    must("SoaDirtyAwarePass<DeadCoercionEliminationPass>", "AC2", impls)
    must("DirtySoAEntryPass<ComputeKindWrap>", "AC2", impls)
    must("#2907", "AC2", cc)

    # AC3
    must("pass_pipeline_concept_rejection_total", "AC3", core)
    must("production-pack-zero-aos-bridge-wired", "AC3", obs)
    must("pass-pipeline-concept-rejection-total", "AC3", obs)

    # AC4
    must("run_production_soa_dirty_hot_pack", "AC4", svc)
    must("production_soa_dirty_hot_pack_invocations_total", "AC4", core)
    must("#2907 AC4", "AC4", test)
    must("run_production_soa_dirty_hot_pack", "AC4", test)

    # AC5
    must("schema-2907", "AC5", obs)
    must("production-soa-dirty-hot-pack-wired", "AC5", obs)
    must("soa-to-aos-bridge-sunset-wired", "AC5", obs)
    must("check_soa_sunset_bridge_2907", "AC5", build)
    must("cmd_soa_sunset_bridge_2907", "AC5", build)
    must("#2907", "AC5", test)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2907-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2907.cpp").is_file():
        fails.append("tests/compiler/test_issue_2907.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2907 sunset SoAtoAoSBridgePass — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
