#!/usr/bin/env python3
"""Issue #3821: synth-hard-fail abort must pair begin fence with force-dirty.

Linear synth-hard-fail arm called abort_ir_cache_begin_force_fn_() then
restored topology and returned without abort_ir_cache_force_dirty_fn_(),
leaving abort_force_in_progress_ stuck. Dual-topology abort sites already
pair begin → restore → force_dirty.

Contract (one row per AC):
  AC1  Synth-hard-fail arm: begin before restore, force_dirty after restore
  AC2  Global begin_force call count == force_dirty call count (no unpaired)
  AC3  Soft dual-topology paired path unchanged (abort_restore sites still pair)
  AC4  force_ir_cache_dirty_after_abort clears abort_force_in_progress_
  AC5  Tests extend test_abort_ir_cache_fence_first; linter in build.py;
       no test_issue_3821.cpp; no docs/design/3821-*

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

    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    svc = _read("src/compiler/service.ixx")
    t = _read("tests/compiler/test_abort_ir_cache_fence_first.cpp")
    build = _read("build.py")

    # ── AC1: synth-hard-fail arm pairs begin → restore → force_dirty ──
    marker = 'force_linear_rollback(composite ? "composite-linear-synth-hard-fail"'
    start = emb.find(marker)
    if start < 0:
        fails.append("AC1: synth-hard-fail force_linear_rollback arm missing")
        win = ""
    else:
        end = emb.find("return cp;", start)
        if end < 0:
            end = start + 4000
        win = emb[start:end]
        must("Issue #3821", "AC1 cite", win)
        must("abort_ir_cache_begin_force_fn_", "AC1 begin fence", win)
        must("abort_ir_cache_force_dirty_fn_", "AC1 force_dirty", win)
        must("abort_restore_dual_topology", "AC1 topology restore", win)
        bpos = win.find("abort_ir_cache_begin_force_fn_")
        rpos = win.find("abort_restore_dual_topology")
        dpos = win.find("abort_ir_cache_force_dirty_fn_")
        if not (bpos >= 0 and rpos >= 0 and dpos >= 0 and bpos < rpos < dpos):
            fails.append("AC1: expected begin < restore < force_dirty order on synth arm")

    # ── AC2: global begin count == force_dirty count (no unpaired latch) ──
    begin_re = re.compile(r"abort_ir_cache_begin_force_fn_\s*\(\s*\)\s*;")
    dirty_re = re.compile(r"abort_ir_cache_force_dirty_fn_\s*\(\s*\)\s*;")
    begins = len(begin_re.findall(emb))
    dirtys = len(dirty_re.findall(emb))
    if begins < 5:
        fails.append(f"AC2: expected ≥5 begin_force calls, found {begins}")
    if dirtys < 5:
        fails.append(f"AC2: expected ≥5 force_dirty calls, found {dirtys}")
    if begins != dirtys:
        fails.append(f"AC2: unpaired fence — begin={begins} force_dirty={dirtys}")

    # ── AC3: Soft dual-topology paired path unchanged ──
    dual_sites = [m.start() for m in re.finditer(r"abort_restore_dual_topology\s*\(", emb)]
    if len(dual_sites) < 3:
        fails.append(f"AC3: expected ≥3 abort_restore_dual_topology sites, found {len(dual_sites)}")
    for i, cs in enumerate(dual_sites):
        before = emb[max(0, cs - 8000) : cs]
        after = emb[cs : cs + 8000]
        if "abort_ir_cache_begin_force_fn_" not in before:
            fails.append(f"AC3: dual-topology site {i + 1} missing begin_force before restore")
        if "abort_ir_cache_force_dirty_fn_" not in after:
            fails.append(f"AC3: dual-topology site {i + 1} missing force_dirty after restore")

    # ── AC4: force_dirty clears abort_force_in_progress_ ──
    fpos = svc.find("void force_ir_cache_dirty_after_abort()")
    if fpos < 0:
        fails.append("AC4: force_ir_cache_dirty_after_abort missing")
    else:
        # Body window covers the #4084 jit_cache_ erase that sits
        # before the in_progress clear.
        body = svc[fpos : fpos + 6000]
        must("abort_force_in_progress_.store(0, std::memory_order_release)", "AC4 clear latch", body)
        must("abort_map_invalid = true", "AC4 abort_map_invalid", body)

    # ── AC5: tests + build wiring; no invent ──
    must("ac3821_1_synth_hard_fail_pairs_force_dirty", "AC5 test fn", t)
    must("3821 AC1: synth-hard-fail pairs force_dirty", "AC5 AC1 cite", t)
    must("ac3821_2_begin_equals_dirty_count", "AC5 pairing count", t)
    must("ac3821_3_force_dirty_clears_in_progress", "AC5 runtime latch", t)
    must("ac3821_4_soak_no_permanent_latch", "AC5 soak", t)
    must("check_synth_hard_fail_abort_force_dirty_3821", "AC5 build.py", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3821.cpp").is_file():
        fails.append("AC5: test_issue_3821.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3821.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3821.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3821-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print(f"Issue #3821 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print(
        "OK: Issue #3821 synth-hard-fail abort pairs begin→force_dirty "
        f"(begin={begins} dirty={dirtys}; dual sites={len(dual_sites)})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
