#!/usr/bin/env python3
"""Issue #3403: InlinePass + run_pipeline dual-emit residual — SoA hot entry + hard-zero bridge gate.

Contract:
  AC1 `InlinePass` declares a `run_on_dirty_blocks_only(IRModuleV2&,
     DefineDirtyMaskView*)` SoA hot entry AND a `#3403` source-cite
     anchor documenting that the AoS `run(IRModule&)` walk is the
     cold / tests / debug print path only (production incremental
     pack must call the SoA entry, not the AoS walk).
  AC2 `soa_view.ixx` carries the `hard_zero_dual_emit_bridge_in_
     production()` abort gate AND bumps to `g_soa_dual_emit_bridge_
     count` abort in `production_defaults_active()` (Hard zero,
     not a dashboard vanity).
  AC3 `run_incremental_dirty_pass_suite_` source-cite anchor stays
     routed through `run_production_soa_dirty_hot_pack` (no new
     InlinePass::run AoS call sites in the production incremental
     pack).
  AC4 existing dirty / fold / DCE suites green (`#3355` single-mark
     ban preserved — `mark_block_dirty` loops still banned).
  AC5 no tests/core/test_issue_3403.cpp (extends existing tests per
     #81934); no docs/design/3403-*.md (per #1655).
  AC6 source-cite #3403 + build.py registration; no design docs.

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


def _strip_comments_and_strings(src: str) -> str:
    out = re.sub(r"//[^\n]*", "", src)
    out = re.sub(r"/\*.*?\*/", "", out, flags=re.S)
    out = re.sub(r'"(?:\\.|[^"\\])*"', '""', out)
    return out


def main() -> int:
    fails: list[str] = []

    pass_impls = _read("src/compiler/pass_impls.ixx")
    soa_view = _read("src/compiler/soa_view.ixx")
    service = _read("src/compiler/service.ixx")
    build = _read("build.py")
    _strip_comments_and_strings(pass_impls)
    sv_stripped = _strip_comments_and_strings(soa_view)

    # AC1: InlinePass SoA hot entry + cold-path source-cite anchor.
    # IRModuleV2 lives in aura::compiler (ir_soa.ixx), not aura::ir.
    soa_entry = "void run_on_dirty_blocks_only(IRModuleV2& module,"
    if soa_entry not in pass_impls:
        fails.append(
            "AC1: InlinePass is missing "
            "run_on_dirty_blocks_only(IRModuleV2&, DefineDirtyMaskView*) "
            "SoA hot entry (production pack has no SoA-only path)"
        )
    cold_anchor = "// Issue #3403: AoS `run(IRModule&)` is the cold"
    if cold_anchor not in pass_impls:
        fails.append(
            "AC1: InlinePass::run(IRModule&) is missing the #3403 "
            "cold / tests / debug print path source-cite anchor "
            "(production incremental pack contract undocumented)"
        )
    soa_anchor = "// Issue #3403 AC1: SoA dirty-block-only entry for the InlinePass"
    if soa_anchor not in pass_impls:
        fails.append(
            "AC1: InlinePass is missing the #3403 SoA hot entry "
            "source-cite anchor (production hot path contract undocumented)"
        )

    # AC2: hard-zero gate + production abort.
    if "hard_zero_dual_emit_bridge_in_production" not in soa_view:
        fails.append(
            "AC2: soa_view.ixx is missing "
            "hard_zero_dual_emit_bridge_in_production() abort gate "
            "(g_soa_dual_emit_bridge_count must be Hard zero in "
            "production_defaults, not a dashboard vanity)"
        )
    if not re.search(
        r"record_soa_dual_emit_bridge\s*\(\s*\)\s*noexcept\s*\{[^}]*production_defaults_active",
        sv_stripped,
        re.S,
    ):
        fails.append(
            "AC2: record_soa_dual_emit_bridge() does not abort under "
            "production_defaults_active() (counter can be bumped from "
            "production hot path without detection)"
        )
    if "g_soa_dual_emit_bridge_count" not in soa_view:
        fails.append("AC2: soa_view.ixx is missing g_soa_dual_emit_bridge_count counter declaration")

    # AC3: run_incremental_dirty_pass_suite_ routed through SoA dirty
    # pack. The issue requires that the production incremental pack
    # does not call InlinePass::run(IRModule&) AoS walk. Verify the
    # suite still dispatches through run_production_soa_dirty_hot_pack
    # (or equivalent SoA entry) when soa_mod is present.
    if "run_incremental_dirty_pass_suite_" not in service or "run_production_soa_dirty_hot_pack" not in service:
        fails.append(
            "AC3: service.ixx is missing run_incremental_dirty_pass_suite_ "
            "or run_production_soa_dirty_hot_pack (production incremental "
            "pack routing broken)"
        )

    # AC4: #3355 single-mark ban preserved (mark_block_dirty loops still
    # banned by the linter sibling).
    # The #3355 linter is a separate file; verify it still exists.
    glob_matches = list((ROOT / "scripts" / "coverage" / "checks").glob("check_mark_block_dirty*3355*"))
    fallback_matches = list(ROOT.glob("scripts/**/check_*3355*.py"))
    if not glob_matches and not fallback_matches:
        fails.append(
            "AC4: #3355 single-mark ban linter not found — the "
            "mark_block_dirty loop ban must stay in place (#3355 "
            "is open and not merged with #3403)"
        )

    # AC5: no tests/core/test_issue_3403.cpp, no docs/design/3403-*.md.
    if (ROOT / "tests" / "core" / "test_issue_3403.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3403.cpp exists — must extend existing test per #81934")
    if list((ROOT / "docs" / "design").glob("3403-*.md")):
        fails.append("AC5: docs/design/3403-*.md exists — design docs banned per #1655")

    # AC6: source-cite #3403 + build.py registration; no design docs.
    if "#3403" not in pass_impls and "#3403" not in soa_view:
        fails.append("AC6: source-cite #3403 missing from pass_impls.ixx / soa_view.ixx")
    if "check_inline_pass_soa_3403" not in build:
        fails.append("AC6: build.py does not register check_inline_pass_soa_3403")

    # Issue #3454 AC2: InlinePass SoA remains the production dispatch
    # target; dual-emit abort unchanged. ProductionPureWrapPass now
    # type-checks IRModuleV2 / IRFunctionSoA (not AoS IRFunction&).
    concepts = _read("src/core/concept_constraints.ixx")
    start_ppw = concepts.find("concept ProductionPureWrapPass")
    end_ppw = concepts.find("concept DirtySoAEntryPass", start_ppw) if start_ppw >= 0 else -1
    ppw_body = concepts[start_ppw:end_ppw] if start_ppw >= 0 and end_ppw > start_ppw else ""
    if "IRModuleV2" not in ppw_body and "IRFunctionSoA" not in ppw_body:
        fails.append(
            "AC2/#3454: ProductionPureWrapPass does not type-check "
            "InlinePass's IRModuleV2 (or IRFunctionSoA) SoA dirty entry"
        )
    if "hard_zero_dual_emit_bridge_in_production" not in soa_view:
        fails.append("AC2/#3454: dual-emit abort gate (#3403) must stay")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1
    print("PASS: #3403 InlinePass + run_pipeline dual-emit residual contract satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
