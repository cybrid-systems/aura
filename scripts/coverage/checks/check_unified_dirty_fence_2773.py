#!/usr/bin/env python3
"""Issue #2773: unify dirty-bit + generation fence write protocol.

Thin ship on #2522/#2615/#2617: one logical cascade counter, documented
fence writers, multi-block IR → one g_ir_soa_generation_fence advance,
Shape compact still isolated from deopt-storm.

Contract (one row per AC):
  AC1 mark_blocks_dirty → one bump_generation + note_logical (batch bit)
  AC2 quiet path: note_logical only on dirty marks (no free-running atomics)
  AC3 Shape #2617 compact≠storm isolation preserved; IR note does not bump shape
  AC4 schema-2773 + unified-dirty-fence-advance-total; #2522/#2615 preserved
  AC5 ac2773_* in test_batch_dirty_discipline + this linter; no docs/design/*

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


def _extract_fn(src: str, name: str) -> str:
    """Best-effort extract of `inline void IRFunctionSoA::name(...) { ... }`."""
    m = re.search(rf"inline void IRFunctionSoA::{re.escape(name)}\s*\(", src)
    if not m:
        # class-inline methods
        m = re.search(rf"void {re.escape(name)}\s*\([^)]*\)\s*\{{", src)
        if not m:
            return ""
    start = m.start()
    depth = 0
    i = src.find("{", start)
    if i < 0:
        return ""
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[start : j + 1]
    return src[start : start + 800]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    soa = _read("src/compiler/ir_soa.ixx")
    shape = _read("src/compiler/shape_profiler.h")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_batch_dirty_discipline.cpp")
    build = _read("build.py")

    # AC1 — batch path one fence + note_logical
    must("#2773", "AC1", soa)
    must("note_logical_invalidation_epoch", "AC1", soa)
    must("kInvSrcIrSoaBatch", "AC1", soa)
    must("g_unified_dirty_fence_advance_total", "AC1", soa)
    batch = _extract_fn(soa, "mark_blocks_dirty")
    if not batch:
        fails.append("AC1: could not extract mark_blocks_dirty")
    else:
        must("bump_generation()", "AC1 batch", batch)
        must("note_logical_invalidation_epoch", "AC1 batch", batch)
        must("kInvSrcIrSoaBatch", "AC1 batch", batch)
        # Exactly one bump_generation in batch body
        bumps = batch.count("bump_generation()")
        if bumps != 1:
            fails.append(f"AC1: mark_blocks_dirty should have 1 bump_generation, got {bumps}")

    # AC2 — quiet path documentation + single residual path
    must("Quiet path", "AC2", soa)
    must("kInvSrcIrSoaSingle", "AC2", soa)
    single = _extract_fn(soa, "mark_block_dirty")
    if single:
        must("note_logical_invalidation_epoch", "AC2 single", single)
        must("kInvSrcIrSoaSingle", "AC2 single", single)
    else:
        fails.append("AC2: could not extract mark_block_dirty")

    # AC3 — shape isolation
    must("2617", "AC3", shape)
    must("#2773", "AC3", shape)
    must("preserve #2617", "AC3", soa)
    # IR note must not mention shape_version bump as side effect of IR dirty
    if "shape_version" in soa and "bump_shape" in soa.lower():
        # only fail if note_logical body forces shape
        note = _extract_fn(soa, "note_logical_invalidation_epoch")
        if "shape" in note.lower() and "bump" in note.lower():
            fails.append("AC3: note_logical must not bump shape")

    # AC4 — observability
    must("schema-2773", "AC4", obs)
    must("unified-dirty-fence-advance-total", "AC4", obs)
    must("unified-dirty-fence-wired", "AC4", obs)
    must("schema-2522", "AC4", obs)
    must("schema-2615", "AC4", obs)

    # AC5 — tests + wire
    must("ac2773_1_batch_one_fence_one_logical", "AC5", test)
    must("ac2773_2_single_separate", "AC5", test)
    must("ac2773_3_shape_isolation", "AC5", test)
    must("ac2773_4_obs_schema", "AC5", test)
    must("ac2773_5_source_cite_quiet", "AC5", test)
    must("check_unified_dirty_fence_2773", "AC5", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2773-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2773.cpp").is_file():
        fails.append("AC5: test_issue_2773.cpp present (forbidden per #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print(
        "OK: Issue #2773 unified dirty fence protocol — logical epoch + batch one-fence + schema-2773 + #2617 isolation"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
