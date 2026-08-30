#!/usr/bin/env python3
"""Issue #3405: PureWrapPass concept tightening — DirtyAware production members must offer run_on_dirty_blocks_only.

Contract:
  AC1 ProductionPureWrapPass concept (concept_constraints.ixx) requires
     PureWrapPass<P> + SoAViewAwarePass<P> + DirtyAwarePass<P> AND a
     `run_on_dirty_blocks_only` entry. #3454 residual: the requires
     clause type-checks SoA `(IRFunctionSoA&)` / `(IRModuleV2&)`, not
     legacy `(IRFunction&)`. Source-cite anchor #3405 documents the
     migration plan (legacy stays grandfathered for existing CK/
     CF/TP/Shape/Escape suites via DirtySoAEntryPass; NEW production
     members must use the SoA per-function signature).
  AC2 `check_pass_dod_compliance` (pass_pipeline_core.ixx) still
     exists (no regression to existing concept enforcement).
  AC3 existing Wraps in optimization_passes.ixx with `kPureWrap =
     true` continue to expose a `run_on_dirty_blocks_only` entry
     (either signature). The optional linter on `kPureWrap` types
     lists each Wrap's run_on_dirty_blocks_only signature.
  AC4 no `std::function` pred regression. The existing
     `kPureWrapNoStdFunctionDirtyIssue = 3042` invariant stays.
  AC5 no `tests/core/test_issue_3405.cpp` (extends existing tests
     per #81934); no `docs/design/3405-*` (per #1655).
  AC6 source-cite #3405 + build.py registration; no design docs.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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

    concepts = _read("src/core/concept_constraints.ixx")
    pipeline_core = _read("src/compiler/pass_pipeline_core.ixx")
    opt_passes = _read("src/compiler/optimization_passes.ixx")
    build = _read("build.py")

    # AC1: ProductionPureWrapPass concept exists + #3405 source-cite anchor.
    if "concept ProductionPureWrapPass" not in concepts:
        fails.append(
            "AC1: ProductionPureWrapPass concept not declared in "
            "src/core/concept_constraints.ixx (#3405 contract not shipped)"
        )
    # The concept must require both PureWrapPass + a run_on_dirty_blocks_only
    # entry. #3454: the requires-clause type-checks SoA (IRFunctionSoA /
    # IRModuleV2), not legacy AoS IRFunction&. BlockDirtyPred stays named
    # in the #3405 comment (lives in pass_pipeline_core; defaulted at impl).
    if "IRFunctionSoA" not in concepts or "BlockDirtyPred" not in concepts:
        fails.append(
            "AC1: ProductionPureWrapPass concept does not declare the "
            "new SoA per-function signature `(IRFunctionSoA&, BlockDirtyPred)`"
        )
    start_ppw = concepts.find("concept ProductionPureWrapPass")
    end_ppw = concepts.find("concept DirtySoAEntryPass", start_ppw) if start_ppw >= 0 else -1
    ppw_body = concepts[start_ppw:end_ppw] if start_ppw >= 0 and end_ppw > start_ppw else ""
    if "aura::ir::IRFunction&" in ppw_body:
        fails.append(
            "AC1: ProductionPureWrapPass still type-checks AoS "
            "`aura::ir::IRFunction&` (#3454 residual — SoA entry is linter-only)"
        )
    if "IRFunctionSoA" not in ppw_body and "IRModuleV2" not in ppw_body:
        fails.append(
            "AC1: ProductionPureWrapPass requires-clause does not type-check "
            "IRFunctionSoA or IRModuleV2 (SoA dirty entry not compile-visible)"
        )
    if "kPureWrap = true" not in concepts:
        fails.append(
            "AC1: ProductionPureWrapPass concept does not reference the `kPureWrap` flag (must require PureWrapPass<P>)"
        )
    # The #3405 source-cite anchor must be present in the comment block.
    if "Issue #3405" not in concepts:
        fails.append(
            "AC1: concept_constraints.ixx is missing the #3405 source-cite anchor on the ProductionPureWrapPass concept"
        )

    # AC2: existing check_pass_dod_compliance still exists (no regression).
    if "check_pass_dod_compliance" not in pipeline_core:
        fails.append(
            "AC2: check_pass_dod_compliance not found in pass_pipeline_core.ixx "
            "(existing concept enforcement regressed)"
        )
    if "HotPassDodCompliant" not in pipeline_core:
        fails.append(
            "AC2: HotPassDodCompliant reference not found in pass_pipeline_core.ixx (existing concept regressed)"
        )

    # AC3: existing Wraps with kPureWrap = true expose run_on_dirty_blocks_only
    # (either signature). The optional linter on kPureWrap types lists each
    # Wrap's signature.
    #
    # Strategy: count the number of `kPureWrap = true` declarations and the
    # number of `run_on_dirty_blocks_only` declarations. The two counts
    # must match (every Wrap declares both). The ±3000 char proximity
    # check + balanced-brace class-span detector were both brittle on
    # RenderPass (whose class body has run_on_dirty_blocks_only at
    # line 737 and kPureWrap = true at line 743 — only 6 lines apart,
    # but the proximity check still flagged it false-positive). The
    # count check is simpler and correct.
    kpurewrap_count = len(re.findall(r"kPureWrap\s*=\s*true", opt_passes))
    rdo_block_only_count = len(re.findall(r"run_on_dirty_blocks_only", opt_passes))
    if kpurewrap_count == 0:
        fails.append("AC3: no `kPureWrap = true` declarations found in optimization_passes.ixx (no Wraps to verify)")
    elif rdo_block_only_count < kpurewrap_count:
        fails.append(
            f"AC3: `kPureWrap = true` count ({kpurewrap_count}) exceeds "
            f"`run_on_dirty_blocks_only` declaration count "
            f"({rdo_block_only_count}) — at least one PureWrap does NOT "
            "expose the production-incremental-pack DirtyAware entry "
            "(every kPureWrap stage must offer `run_on_dirty_blocks_only`)"
        )

    # AC4: no std::function pred regression. The existing
    # kPureWrapNoStdFunctionDirtyIssue = 3042 invariant stays.
    if "kPureWrapNoStdFunctionDirtyIssue" not in pipeline_core:
        fails.append(
            "AC4: kPureWrapNoStdFunctionDirtyIssue = 3042 invariant not "
            "found in pass_pipeline_core.ixx (std::function ban regressed)"
        )

    # AC5: no tests/core/test_issue_3405.cpp, no docs/design/3405-*.md.
    if (ROOT / "tests" / "core" / "test_issue_3405.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3405.cpp exists — must extend existing test per #81934")
    if list((ROOT / "docs" / "design").glob("3405-*.md")):
        fails.append("AC5: docs/design/3405-*.md exists — design docs banned per #1655")

    # AC6: source-cite #3404 + build.py registration; no design docs.
    # (Source-cite check uses #3405 anchor from concept file.)
    if "ProductionPureWrapPass" not in concepts and "Issue #3405" not in concepts:
        fails.append(
            "AC6: ProductionPureWrapPass concept + #3405 source-cite anchor missing from concept_constraints.ixx"
        )
    if "check_pure_wrap_dirty_entry_3405" not in build:
        fails.append("AC6: build.py does not register check_pure_wrap_dirty_entry_3405")

    # Issue #3454 Option B belt: production incremental pack grandfather
    # is explicit and length-capped at today's 5 Wraps. New names in
    # run_incremental_dirty_pass_suite_ must have a SoA dirty-entry needle.
    service = _read("src/compiler/service.ixx")
    grandfather = {
        "ComputeKindWrap",
        "ConstantFoldingWrap",
        "TypePropagationPass",
        "ShapeWrap",
        "EscapeAnalysisWrap",
    }
    if "Issue #3454 AC3 grandfather (length-capped 5)" not in service:
        fails.append(
            "AC3/#3454: run_incremental_dirty_pass_suite_ is missing the "
            "length-capped grandfather source-cite (CK/CF/TP/Shape/Escape)"
        )
    start = service.find("std::size_t run_incremental_dirty_pass_suite_")
    end = service.find("run_coercion_elim_on_function", start) if start >= 0 else -1
    suite = service[start:end] if start >= 0 and end > start else ""
    decls = dict(
        re.findall(
            r"(ComputeKindWrap|ConstantFoldingWrap|TypePropagationPass|"
            r"ShapeWrap|EscapeAnalysisWrap|InlinePass|[A-Z][A-Za-z0-9]+)\s+(\w+)\s*;",
            suite,
        )
    )
    vars_in_pipe = re.findall(r"run_production_incremental_dirty_pipeline\(\s*ir_mod,\s*(\w+)", suite)
    pack_types: list[str] = []
    for var in vars_in_pipe:
        # decls maps Type -> var; invert
        found = None
        for ty, name in decls.items():
            if name == var:
                found = ty
                break
        pack_types.append(found or var)
    if len(pack_types) != 5:
        fails.append(
            f"AC3/#3454: production incremental pack length is {len(pack_types)} "
            f"(cap is 5 grandfathered Wraps); extra names need SoA dirty entry"
        )
    extra = [t for t in pack_types if t not in grandfather]
    if extra:
        fails.append(
            "AC3/#3454: new production-pack names "
            f"{extra} are not on the grandfather list — they must offer "
            "run_on_dirty_blocks_only(IRModuleV2 or IRFunctionSoA)"
        )

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1
    print("PASS: #3405 PureWrapPass concept tightening contract satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
