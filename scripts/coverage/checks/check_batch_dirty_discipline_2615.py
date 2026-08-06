#!/usr/bin/env python3
"""Issue #2615 + #2681: production multi-block dirty cascades use batch mark_blocks_dirty.

Contract (#2615 baseline):
  AC1 DCE / impact_scope use mark_blocks_dirty / mark_blocks_dirty_bit_only
  AC2 mark_block_dirty retained for single-block (single marks counter)
  AC3 mark_blocks_dirty_bits_only + finish_dirty_sync path
  AC4 residual multi mark_block_dirty loops banned in hot production files
  AC5 schema-2615 fence metrics + test/cmake/build gate

Contract (#2681 harden):
  AC1 (re-asserted) batch APIs are the only production multi-block path
  AC2 (re-asserted) single-block mark unchanged (counter + fence +1)
  AC3 ALL production TUs under src/compiler/ scanned (not just 4 HOT_FILES)
  AC4 mark_all_blocks_dirty still bulk fill + single bump (no regression)
  AC5 soa-batch-blocks-per-cascade-bp derived query key present (basis points)
  AC6 self-coverage: schema-2681 / issue-2681 / hardened sentinel present

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# Legacy 4-file HOT_FILES list (#2615 baseline). #2681 widens scan to ALL prod TUs.
HOT_FILES_LEGACY = [
    "src/compiler/pass_impls.ixx",
    "src/compiler/service.ixx",
    "src/compiler/service_dirty.cpp",
    "src/compiler/dirty_propagation.ixx",
]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _scan_residual_loops(content: str, rel: str) -> list[str]:
    """Return list of failing snippets for residual multi-block mark_block_dirty loops.

    The residual loop pattern is: a for/while loop whose body contains a bare
    `mark_block_dirty(<id>)` call (not the batch variants, not the no-bump
    helper, not the impl dispatcher).
    """
    fails: list[str] = []
    # Strip comments so doc-comments don't false-positive.
    stripped = re.sub(r"//[^\n]*", "", content)
    residual_pat = re.compile(
        r"(?:for|while)\s*\([^;]{0,200}\)\s*\{[^{}]{0,1500}?\bmark_block_dirty\s*\(",
        re.MULTILINE | re.DOTALL,
    )
    for m in residual_pat.finditer(stripped):
        snippet = m.group(0)
        # Strip the batch / no-bump / impl variants so a "bare" mark_block_dirty
        # call stands out.
        bare = re.sub(r"\bmark_block_dirty_(?:bit_only_no_bump|no_bump|impl|bits_only|bit_only)\b", "", snippet)
        bare = re.sub(r"\bmark_blocks_dirty\b", "", bare)
        if re.search(r"\bmark_block_dirty\s*\(", bare):
            fails.append(f"AC3/AC4 (#2681): residual multi-block mark_block_dirty loop in {rel}: {snippet[:200]!r}...")
    return fails


def _iter_prod_files() -> list[Path]:
    """Walk src/compiler/ and return every production TU (excluding tests)."""
    out: list[Path] = []
    src = ROOT / "src" / "compiler"
    if not src.is_dir():
        return out
    for p in src.rglob("*"):
        if not p.is_file():
            continue
        if "tests" in p.parts:
            continue
        if p.suffix not in (".cpp", ".ixx", ".hh", ".h"):
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
    dce = _read("src/compiler/pass_impls.ixx")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_batch_dirty_discipline.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # ── #2615 baseline ──

    # AC1
    must("Issue #2615", "AC1", soa)
    must("mark_blocks_dirty_bits_only", "AC1", soa)
    must("g_ir_soa_batch_dirty_cascades_total", "AC1", soa)
    must("kIrSoaBatchDirtyDisciplineIssue", "AC1", soa)
    must("Issue #2615", "AC1", dce)
    must("mark_blocks_dirty(changed_blocks)", "AC1", dce)
    must("mark_blocks_dirty_bit_only", "AC1", svc)
    must("ac1_multi_batch", "AC1", test)

    # AC2
    must("g_ir_soa_single_dirty_marks_total", "AC2", soa)
    must("ac2_single_unchanged", "AC2", test)

    # AC3 (#2615: path-level bits-only + finish sync)
    must("mark_block_dirty_bit_only_no_bump", "AC3", soa)
    must("ac3_finish_sync", "AC3", test)

    # AC4 (#2615: HOT_FILES residual loop ban + explicit DCE block)
    for rel in HOT_FILES_LEGACY:
        hay = _read(rel)
        if not hay:
            continue
        fails.extend(_scan_residual_loops(hay, rel))
    if "mark_block_dirty(block.block_id)" in dce:
        fails.append("AC4: pass_impls still has mark_block_dirty(block.block_id)")
    must("ac4_no_residual_loops", "AC4", test)

    # AC5 (#2615: schema + cmake + build gate)
    must("schema-2615", "AC5", q)
    must("soa-batch-dirty-cascades-total", "AC5", q)
    must("soa-dirty-fence-total", "AC5", q)
    must("test_batch_dirty_discipline", "AC5", cmake)
    must("check_batch_dirty_discipline_2615", "AC5", build)
    must("cmd_batch_dirty_discipline_coverage", "AC5", build)
    must("ac5_fence_rate", "AC5", test)

    # No design docs (per #1655).
    for rel in (
        "docs/design/batch_dirty_discipline_2615.md",
        "docs/design/batch_dirty_discipline_2681.md",
        "docs/batch_dirty_discipline_2615.md",
        "design/2615.md",
        "design/2681.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC5/AC6: unexpected design doc {rel}")

    # ── #2681 harden ──

    # AC3 (#2681 widen): ALL production TUs in src/compiler/ scanned, not just HOT_FILES.
    for p in _iter_prod_files():
        rel = str(p.relative_to(ROOT))
        try:
            content = p.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue
        fails.extend(_scan_residual_loops(content, rel))

    # AC4 (#2681 harden): mark_all_blocks_dirty still bulk + single bump.
    # In ir_soa.ixx: std::fill(block_dirty_...) + std::fill(instruction_dirty_...)
    # + ONE bump_generation() call. No second bump. The body has inline
    # brace init (`std::uint8_t{1}`) which the naive [^{}]* regex skips, so
    # we use a manual brace-depth walker.
    sig = re.search(r"void\s+mark_all_blocks_dirty\s*\(\s*\)\s*\{", soa)
    if not sig:
        fails.append("AC4 (#2681): mark_all_blocks_dirty impl missing in ir_soa.ixx")
    else:
        start = sig.end()
        depth = 1
        i = start
        while i < len(soa) and depth > 0:
            c = soa[i]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
            i += 1
        body = soa[start : i - 1]
        bumps = len(re.findall(r"\bbump_generation\s*\(\s*\)", body))
        if bumps != 1:
            fails.append(f"AC4 (#2681): mark_all_blocks_dirty has {bumps} bump_generation() calls, expected 1")

    # AC5 (#2681 harden): derived bp key + schema/issue/hardened sentinels.
    must("soa-batch-blocks-per-cascade-bp", "AC5-2681", q)
    must("schema-2681", "AC5-2681", q)
    must("issue-2681", "AC5-2681", q)
    must("soa-batch-dirty-discipline-hardened", "AC5-2681", q)
    must("ac2681_blocks_per_cascade_bp", "AC5-2681", test)

    # AC6 (#2681 self-coverage): #2681 sentinel in ir_soa.ixx + production
    # cascade files + linter self-reference + build.py wired. Check for "#2681"
    # (not "Issue #2681") so combined citations like "Issue #2615 / #2681"
    # still match without forcing a separate explicit "Issue #2681" line.
    must("#2681", "AC6", soa)
    must("#2681", "AC6", dce)
    must("#2681", "AC6", svc)
    must("#2681", "AC6", q)
    must("ac2681_source_cite", "AC6", test)
    must("check_batch_dirty_discipline_2615", "AC6", build)  # linter already wired

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2615/#2681 batch dirty cascade discipline — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
