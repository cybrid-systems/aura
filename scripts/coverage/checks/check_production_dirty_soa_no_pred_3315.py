#!/usr/bin/env python3
"""Issue #3315: production PureWrap dirty path forces DirtySoAEntry / columnar
mask — no residual set_block_dirty_pred (I5 of #2258/#3042/#2907).

#3042 banned std::function dirty members. Residual: production still called
set_block_dirty_pred even when DirtySoAEntryPass (run_on_dirty_blocks_only)
exists, leaving a runtime pred surface that could re-scan clean blocks.

Contract:
  AC1  Production dirty path uses DirtySoAEntry / columnar BlockDirtyPred;
       run_production_soa_dirty_hot_pack never calls set_block_dirty_pred;
       no capturing lambda / no std::function.
  AC2  Clean blocks not visited under a partial dirty mask (pipeline skip).
  AC3  Soft / unit / no-mask: setter remains for tests; zero extra.
  AC4  Linter after #3042; test_hot_pass_hard_dod; no invent / docs/design.

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

    core = _read("src/compiler/pass_pipeline_core.ixx")
    impls = _read("src/compiler/pass_impls.ixx")
    opt = _read("src/compiler/optimization_passes.ixx")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_hot_pass_hard_dod.cpp")
    soa_test = _read("tests/compiler/test_soa_dirty_aware_pipeline.cpp")
    build = _read("build.py")

    # ── AC1 ──
    must("Issue #3315", "AC1 pipeline cite", core)
    must("kProductionDirtySoaNoPredIssue = 3315", "AC1 stamp", core)
    must("kDirtySoaColumnarEntry", "AC1 skip setter when 2-arg", core)
    must("run_on_dirty_blocks_only(f, pred)", "AC1 2-arg entry", core)
    must("BlockDirtyPred pred = {}", "AC1 CF 2-arg", impls)
    must("zero set_block_dirty_pred", "AC1 production pack comment", impls)
    pack = impls.find("run_production_soa_dirty_hot_pack")
    if pack < 0:
        fails.append("AC1: run_production_soa_dirty_hot_pack missing")
    else:
        end = impls.find("\nexport ", pack + 1)
        body = impls[pack : end if end > pack else pack + 1200]
        if "set_block_dirty_pred" in body:
            fails.append("AC1: production pack calls set_block_dirty_pred")
        if "run_dirty_pipeline" not in body:
            fails.append("AC1: production pack must use run_dirty_pipeline")
    if re.search(r"set_block_dirty_pred\s*\(\s*\[", core + impls + opt):
        fails.append("AC1: capturing lambda passed to set_block_dirty_pred")
    if "std::function" in core and re.search(r"set_block_dirty_pred\s*\(\s*std::function", core + impls + opt):
        fails.append("AC1: std::function passed to set_block_dirty_pred")
    must("3315 AC1", "AC1 test", test)
    must("schema-3315", "AC1 query", q)
    must("run_production_soa_dirty_hot_pack", "AC1 soa test", soa_test)

    # ── AC2 ──
    must("dirty_only_blocks_skipped_total", "AC2 skip counter", core)
    must("3315 AC2", "AC2 test", test)

    # ── AC3 ──
    must("set_block_dirty_fn(bool (*fn)(std::uint32_t))", "AC3 test setter", impls)
    must("3315 AC3", "AC3 test", test)
    must("setter stays", "AC3 comment", impls)

    # ── AC4 ──
    must("check_production_dirty_soa_no_pred_3315", "AC4 build.py", build)
    must("3315 AC4", "AC4 test", test)
    must("Issue #3042", "AC4 3042 lineage", core)
    prev = build.find("check_pure_wrap_no_std_function_3042")
    ours = build.find("check_production_dirty_soa_no_pred_3315")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC4: linter must be wired in build.py AFTER #3042")
    if (ROOT / "tests" / "compiler" / "test_issue_3315.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3315.cpp per #81967")
    if (ROOT / "tests" / "core" / "test_issue_3315.cpp").is_file():
        fails.append("AC4: forbidden tests/core/test_issue_3315.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3315-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")
    if "g_3315_" in core or "g_3315_" in impls:
        fails.append("AC4: invented g_3315_* runtime counter")

    if fails:
        print("FAIL #3315 production_dirty_soa_no_pred:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3315 production_dirty_soa_no_pred: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
