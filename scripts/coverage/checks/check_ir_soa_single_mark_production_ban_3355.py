#!/usr/bin/env python3
"""Issue #3355: production TU hard-ban residual SoA single-mark.

#3293 made residual multi-via-single machine-checkable (streak==2 → abort
under production_defaults). The remaining hole is that IRFunctionSoA::
mark_block_dirty stayed callable from production TUs (dual-emit residual).
A site that never reaches streak==2 still leaves residual single-mark
under the Soft production face.

This linter DISCOVERS every one-arg SoA `.mark_block_dirty(id)` /
`->mark_block_dirty(id)` in src/compiler. Naked production sites fail.
Soft/test keep the API (#3293 SIGABRT fixture + metric-only residual).
Two-arg IRCacheEntry::mark_block_dirty(fi, bi) is a different overload
(dual-emit now batch-of-1).

Contract:
  AC1  production dual-emit uses mark_blocks_dirty of 1; SoA single-mark
       unreachable in production TUs
  AC2  Soft / Off / unit: residual cascade metric-only; hard-abort
       counter untouched (no behavioural change)
  AC3  #3293 live SIGABRT fixture still present
  AC4  every one-arg SoA single-mark site classified production-ban vs
       Soft/test EXEMPT; naked production site fails
  AC5  extend test_batch_dirty_discipline; linter AFTER #3293; no
       test_issue_3355.cpp; no docs/design/; no schema-3355 / g_3355_*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# One-arg SoA single-mark: `.mark_block_dirty(id)` / `->mark_block_dirty(id)`.
# Two-arg IRCacheEntry::mark_block_dirty(fi, bi) does not match (comma).
SINGLE_MARK_RE = re.compile(r"(?:\.|->)mark_block_dirty\s*\(\s*[^,)\n]+\s*\)")


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", src)


def _is_exempt(pre: str) -> bool:
    return "SINGLE_MARK_EXEMPT" in pre[-900:]


def _iter_prod_files() -> list[Path]:
    out: list[Path] = []
    src = ROOT / "src"
    if not src.is_dir():
        return out
    for p in src.rglob("*"):
        if not p.is_file() or p.suffix not in (".cpp", ".ixx", ".hh", ".h"):
            continue
        if "tests" in p.parts:
            continue
        out.append(p)
    return out


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    soa = _read("src/compiler/ir_soa.ixx")
    svc = _read("src/compiler/service.ixx")
    t = _read("tests/compiler/test_batch_dirty_discipline.cpp")
    build = _read("build.py")

    # AC1 — production dual-emit uses batch-of-1; stamp + comment.
    must("kIrSoaSingleMarkProductionBanIssue = 3355", "AC1 stamp", soa)
    must("Issue #3355", "AC1 soa cite", soa)
    must("production TU hard-ban", "AC1 ban comment", soa)
    must("Issue #3355", "AC1 dual-emit cite", svc)
    must("mark_blocks_dirty(one)", "AC1 dual-emit batch-of-1", svc)
    if "functions[func_idx].mark_block_dirty(block_idx)" in svc:
        fails.append("AC1: dual-emit still calls SoA mark_block_dirty(block_idx)")

    # AC2 — Soft residual metric-only preserved (no new abort on streak==1).
    must("g_ir_soa_residual_multi_via_single_cascades_total.fetch_add", "AC2 cascade metric", soa)
    must("g_ir_soa_residual_multi_via_single_marks_total.fetch_add", "AC2 marks metric", soa)
    if "g_3355_" in soa or "g_3355_" in svc:
        fails.append("AC2: new g_3355_* counter")
    must("ac3355_2_soft_metric_only", "AC2 test", t)

    # AC3 — #3293 fixture remains.
    must("ac3293_1_production_hard_face", "AC3 3293 SIGABRT fixture", t)
    must("std::abort()", "AC3 abort path", soa)

    # AC4 — enumerate every one-arg SoA single-mark in production TUs.
    sites = 0
    for p in _iter_prod_files():
        raw = p.read_text(encoding="utf-8", errors="replace")
        stripped = _strip_comments(raw)
        rel = str(p.relative_to(ROOT)).replace("\\", "/")
        for m in SINGLE_MARK_RE.finditer(stripped):
            sites += 1
            pre = stripped[max(0, m.start() - 900) : m.start()]
            if _is_exempt(pre):
                continue
            fails.append(f"AC4: naked production single-mark in {rel}: {m.group(0)!r}")
    must("ac3355_4_linter_enumerates", "AC4 test", t)

    # AC5 — linter after #3293; no invent.
    must("check_ir_soa_single_mark_production_ban_3355", "AC5 build.py", build)
    must("ac3355_1_production_batch_of_one", "AC5 AC1 test", t)
    must("ac3355_5_source_and_linter", "AC5 AC5 test", t)
    prev = build.find("check_ir_dirty_batch_only_production_default_3293")
    ours = build.find("check_ir_soa_single_mark_production_ban_3355")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3293")
    if "schema-3355" in soa or "schema-3355" in svc:
        fails.append("AC5: new schema-3355 query key")
    if _read("tests/compiler/test_issue_3355.cpp"):
        fails.append("AC5: test_issue_3355.cpp present (forbidden #81967)")
    if _read("tests/issues/test_issue_3355.cpp"):
        fails.append("AC5: tests/issues/test_issue_3355.cpp present (forbidden #81967)")
    if _read("docs/design/3355-ir-soa-single-mark-production-ban.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3355 ir_soa_single_mark_production_ban:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print(f"OK #3355 ir_soa_single_mark_production_ban: 0 naked production single-mark sites (scanned hits={sites})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
