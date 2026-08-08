#!/usr/bin/env python3
"""Issue #2774: production multi-block dirty cascade — batch only.

Residual loops of mark_block_dirty on the same function cause N fence
advances. Batch APIs already exist (#2522/#2615/#2681). This issue adds
runtime residual multi-via-single Soft metric + hard static ban.

Contract (one row per AC):
  AC1 production multi-block uses mark_blocks_dirty / bits_only only
      (residual loop scan + batch clears streak)
  AC2 single mark_block_dirty remains (AC2 path; residual not tripped)
  AC3 empty span quiet (no bump / no residual)
  AC4 residual counters + schema-2774; #2522/#2615 preserved
  AC5 ac2774_* + this linter; no docs/design/*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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
    test = _read("tests/compiler/test_batch_dirty_discipline.cpp")
    build = _read("build.py")

    # AC1 — residual metric + static production scan
    must("#2774", "AC1", soa)
    must("g_ir_soa_residual_multi_via_single_cascades_total", "AC1", soa)
    must("note_single_mark_for_residual", "AC1", soa)
    must("clear_single_mark_residual", "AC1", soa)
    must("mark_blocks_dirty", "AC1", soa)
    # Production TUs: no residual loops (lineage #2681)
    src = ROOT / "src" / "compiler"
    for p in sorted(src.rglob("*")):
        if not p.is_file() or p.suffix not in (".cpp", ".ixx", ".hh", ".h"):
            continue
        if "tests" in p.parts:
            continue
        rel = str(p.relative_to(ROOT)).replace("\\", "/")
        fails.extend(_scan_residual_loops(p.read_text(encoding="utf-8", errors="replace"), rel))

    # AC2 — single path retained
    must("g_ir_soa_single_dirty_marks_total", "AC2", soa)
    must("mark_block_dirty", "AC2", soa)
    must("ac2774_2_single_allowed", "AC2", test)

    # AC3 — quiet empty span
    must("AC3 quiet", "AC3", soa)
    must("ac2774_3_empty_quiet", "AC3", test)

    # AC4 — observability
    must("schema-2774", "AC4", obs)
    must("soa-residual-multi-via-single-cascades-total", "AC4", obs)
    must("soa-residual-multi-via-single-marks-total", "AC4", obs)
    must("soa-residual-multi-via-single-ban-wired", "AC4", obs)
    must("schema-2522", "AC4", obs)
    must("schema-2615", "AC4", obs)
    must("ac2774_4_residual_trips_and_schema", "AC4", test)

    # AC5 — tests + wire
    must("ac2774_1_batch_no_residual", "AC5", test)
    must("ac2774_5_source_cite", "AC5", test)
    must("check_batch_dirty_multi_via_single_ban_2774", "AC5", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2774-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2774.cpp").is_file():
        fails.append("AC5: test_issue_2774.cpp present (forbidden per #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2774 residual multi-via-single ban — batch-only multi-block + Soft residual metric + schema-2774")
    return 0


if __name__ == "__main__":
    sys.exit(main())
