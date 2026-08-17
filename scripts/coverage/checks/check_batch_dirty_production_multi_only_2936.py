#!/usr/bin/env python3
"""Issue #2936: production multi-block IR dirty must use batch API only.

Mirrors #2618 residual production smoke for multi-via-single residual
(#2774 Soft metric). Production packs hard-expect residual multi-via-single
cascades/marks == 0 under SoA-only batch workload; true single mark_block_dirty
remains valid (streak==1). Optional AURA_IR_DIRTY_BATCH_ONLY=1 hard-aborts
on residual; Soft/unit residual exercise leaves env unset.

Contract (one row per AC):
  AC1 production multi-block cascade sites use batch APIs; residual smoke
      expects residual multi-via-single == 0 (schema-2936)
  AC2 single mark_block_dirty remains valid (streak 1 never residual cascade)
  AC3 empty span batch quiet (no fence) per #2522 AC3
  AC4 additive observability (production-smoke-wired + schema-2936);
      #2615/#2773/#2774 counters preserved
  AC5 Soft/unit intentional residual still works when batch-only hard off
  AC6 coverage linter + src/-aligned suite (#81967); no docs/design/

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _scan_residual_loops(content: str, rel: str) -> list[str]:
    fails: list[str] = []
    stripped = re.sub(r"//[^\n]*", "", content)
    residual_pat = re.compile(
        r"(?:for|while)\s*\([^;]{0,200}\)\s*\{[^{}]{0,1500}?\bmark_block_dirty\s*\(",
        re.MULTILINE | re.DOTALL,
    )
    for m in residual_pat.finditer(stripped):
        snippet = m.group(0)
        bare = re.sub(
            r"\bmark_block_dirty_(?:bit_only_no_bump|no_bump|impl|bits_only|bit_only)\b",
            "",
            snippet,
        )
        bare = re.sub(r"\bmark_blocks_dirty\b", "", bare)
        if re.search(r"\bmark_block_dirty\s*\(", bare):
            fails.append(f"AC1: residual multi-block mark_block_dirty loop in {rel}")
    return fails


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    soa = _read("src/compiler/ir_soa.ixx")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    dce = _read("src/compiler/pass_impls.ixx")
    svc = _read("src/compiler/service.ixx")
    test = _read("tests/compiler/test_batch_dirty_discipline.cpp")
    build = _read("build.py")

    # ── AC1: production batch-only + residual smoke wire ──
    must("#2936", "AC1", soa)
    must("kSchemaResidualMultiViaSingleProductionSmoke", "AC1", soa)
    must("ir_dirty_batch_only_production_smoke_wired", "AC1", soa)
    must("AURA_IR_DIRTY_BATCH_ONLY", "AC1", soa)
    # Issue #3105: hard-fail observability counter + constant in ir_soa.ixx.
    must("g_ir_soa_batch_only_hard_abort_total", "AC1 #3105", soa)
    must("kIrSoaBatchOnlyHardAbortIssue", "AC1 #3105", soa)
    must("= 3105", "AC1 #3105", soa)
    # Issue #3105: hard-fail path must call std::abort + bump counter (not
    # just log). Source-cite: the abort path in note_single_mark_for_residual
    # must increment the dedicated counter before std::abort.
    abort_window = re.search(
        r"ir_dirty_batch_only_hard\(\)\s*\{[^}]{0,400}?g_ir_soa_batch_only_hard_abort_total\.fetch_add\("
        r"[^}]{0,400}?std::abort\(\)",
        soa,
        re.DOTALL,
    )
    if not abort_window:
        fails.append(
            "AC1 #3105: hard-fail path in note_single_mark_for_residual must "
            "bump g_ir_soa_batch_only_hard_abort_total then std::abort()"
        )
    must("mark_blocks_dirty", "AC1", soa)
    must("mark_blocks_dirty_bits_only", "AC1", soa)
    must("mark_all_blocks_dirty", "AC1", soa)
    must("mark_blocks_dirty", "AC1 DCE", dce)
    must("mark_blocks_dirty", "AC1 service", svc)
    must("ac2936_1_production_smoke_residual_zero", "AC1", test)
    # Production TUs: no residual for-loops of mark_block_dirty (lineage #2681/#2774)
    src = ROOT / "src" / "compiler"
    for p in sorted(src.rglob("*")):
        if not p.is_file() or p.suffix not in (".cpp", ".ixx", ".hh", ".h"):
            continue
        if "tests" in p.parts:
            continue
        rel = str(p.relative_to(ROOT)).replace("\\", "/")
        fails.extend(_scan_residual_loops(p.read_text(encoding="utf-8", errors="replace"), rel))

    # ── AC2: single allowed ──
    must("g_ir_soa_single_dirty_marks_total", "AC2", soa)
    must("mark_block_dirty", "AC2", soa)
    must("ac2936_2_single_allowed", "AC2", test)
    must("streak==1 never bumps residual cascade", "AC2", test)

    # ── AC3: empty span quiet ──
    must("AC3 quiet", "AC3", soa)
    must("ac2936_3_empty_span_quiet", "AC3", test)

    # ── AC4: observability + lineage ──
    must("schema-2936", "AC4", obs)
    must("issue-2936", "AC4", obs)
    must("soa-residual-multi-via-single-production-smoke-wired", "AC4", obs)
    must("ir-dirty-batch-only-production-smoke-wired", "AC4", obs)
    must("schema-2774", "AC4", obs)
    must("schema-2615", "AC4", obs)
    must("schema-2773", "AC4", obs)
    must("soa-residual-multi-via-single-cascades-total", "AC4", obs)
    must("soa-residual-multi-via-single-marks-total", "AC4", obs)
    must("ac2936_4_obs_schema", "AC4", test)

    # ── AC5: Soft residual still works ──
    must("ir_dirty_batch_only_hard", "AC5", soa)
    must("ac2936_5_soft_residual_still_works", "AC5", test)
    must("AURA_IR_DIRTY_BATCH_ONLY", "AC5", test)

    # ── AC6: tests + build + no design ──
    must("ac2936_1_production_smoke_residual_zero", "AC6", test)
    must("ac2936_2_single_allowed", "AC6", test)
    must("ac2936_3_empty_span_quiet", "AC6", test)
    must("ac2936_4_obs_schema", "AC6", test)
    must("ac2936_5_soft_residual_still_works", "AC6", test)
    must("ac2936_6_linter_and_no_design", "AC6", test)
    must("check_batch_dirty_production_multi_only_2936", "AC6", build)
    must("cmd_batch_dirty_production_multi_only_2936", "AC6", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2936-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2936.cpp").is_file():
        fails.append("AC6: test_issue_2936.cpp present (forbidden #81967)")

    # Optional: run standalone discipline binary if present (batch suite is
    # multi-member and may fail for unrelated reasons — do not auto-run it).
    for build_dir in ("build", "build_asan", "build_release"):
        binary = ROOT / build_dir / "test_batch_dirty_discipline"
        if not binary.is_file():
            continue
        env = os.environ.copy()
        env.pop("AURA_IR_DIRTY_BATCH_ONLY", None)
        r = subprocess.run(
            [str(binary)],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            timeout=180,
            check=False,
        )
        if r.returncode != 0:
            fails.append(
                f"AC1: test_batch_dirty_discipline failed under production multi-only "
                f"smoke (exit {r.returncode}). stderr:\n{(r.stderr or '')[-1500:]}"
            )
        break

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print(
        "OK: Issue #2936 production multi-block dirty batch-only — "
        "residual multi-via-single smoke + Soft residual preserved + schema-2936"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
