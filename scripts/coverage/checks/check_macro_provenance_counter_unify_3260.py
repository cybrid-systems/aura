#!/usr/bin/env python3
"""Issue #3260: reconcile #1908 file-level vs per-eval provenance counters.

clone_macro_body must not bump hygiene_violation_prevented (was_violation=0).
Per-eval steal/flush/panic sites dual-write file-level C-API bump accessors
so aura_*_total() matches CompilerMetrics. Stub keeps process-wide atomics
(not a hard-zero that looks like no clones). Soft clone path is one hook
call. No new query:* key.

Contract:
  AC1  clone / was_violation=0 does not bump hygiene file-level
  AC2  3 per-eval sites dual-write bump accessors; was_violation bumps hygiene
  AC3  clone still one hook; was_violation=0 (zero extra hygiene)
  AC4  stub process-wide atomics (Bug 3)
  AC5  extend test_clone_provenance_per_evaluator; linter after #3259; no invent

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

    hdr = _read("src/compiler/aura_jit_bridge.h")
    bridge = _read("src/compiler/aura_jit_bridge.cpp")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    rt = _read("src/compiler/runtime_bridge_stub.cpp")
    me = _read("src/compiler/macro_expansion.cpp")
    fiber = _read("src/compiler/evaluator_fiber_mutation.cpp")
    test = _read("tests/compiler/test_clone_provenance_per_evaluator.cpp")
    build = _read("build.py")
    l2810 = _read("scripts/coverage/checks/check_clone_provenance_per_evaluator_2810.py")

    must("was_violation", "AC1 hook param", hdr)
    must("aura_bump_hygiene_violation_prevented_on_boundary_total", "AC1 accessor", hdr)
    must("aura_hygiene_violation_prevented_on_boundary_total", "AC1 reader", hdr)
    hw = bridge.find("int aura_macro_provenance_repin_on_steal(")
    hwin = bridge[hw : hw + 1600] if hw >= 0 else ""
    must("if (was_violation)", "AC1 gated hygiene", hwin)
    must("Issue #3260", "AC1 hook cite", hwin)
    must("/*was_violation=*/0", "AC1 clone", me)
    must("ac3260_1_clone_does_not_bump_hygiene", "AC1 test", test)

    must("aura_bump_macro_provenance_repin_on_steal_total", "AC2 repin bump", hdr)
    must("aura_bump_hygiene_violation_prevented_on_boundary_total(1)", "AC2 fiber hygiene", fiber)
    must("aura_bump_macro_provenance_repin_on_steal_total(1)", "AC2 fiber repin", fiber)
    flush = fiber.find("outermost Guard exit enforced hygiene")
    fwin = fiber[flush : flush + 900] if flush >= 0 else ""
    must("aura_bump_hygiene_violation_prevented_on_boundary_total(1)", "AC2 flush", fwin)
    if "aura_bump_macro_provenance_repin_on_steal_total(1)" in fwin:
        fails.append("AC2: flush must not dual-write repin (per-eval hygiene-only site)")
    xfer = fiber.find("PanicCheckpoint transfer bound macro clone")
    xwin = fiber[xfer : xfer + 900] if xfer >= 0 else ""
    must("aura_bump_macro_provenance_repin_on_steal_total(1)", "AC2 transfer repin", xwin)
    must("aura_bump_hygiene_violation_prevented_on_boundary_total(1)", "AC2 transfer hygiene", xwin)
    steal = fiber.find("post-steal repin MacroIntroduced boundary")
    swin = fiber[steal : steal + 700] if steal >= 0 else ""
    must("aura_bump_macro_provenance_repin_on_steal_total(1)", "AC2 steal repin", swin)
    must("aura_bump_hygiene_violation_prevented_on_boundary_total(1)", "AC2 steal hygiene", swin)
    must("ac3260_2_violation_and_accessor_mirror", "AC2 test", test)
    must("ac3261_2_hygiene_hoisted_off_dirty_fn", "AC2 3261 hoist", test)

    must("ac3260_3_soft_clone_zero_extra_hygiene", "AC3 test", test)
    must("was_violation=0", "AC3 clone comment", me)

    must("g_1908_repin_stub_total", "AC4 stub atomic", stub)
    must("ac3260_4_stub_not_hard_zero", "AC4 test", test)
    if "return 0; // file-level fallback lives in full bridge only" in stub:
        fails.append("AC4: stub total still hard-zero")

    must("ac3260_5_source_and_linter", "AC5 test", test)
    must("check_macro_provenance_counter_unify_3260", "AC5 build.py", build)
    prev = build.find("check_restamp_hot_cone_budget_3259")
    ours = build.find("check_macro_provenance_counter_unify_3260")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3259")
    must("3260", "AC5 extend 2810 linter", l2810)
    must("Issue #3260", "AC5 runtime stub", rt)
    if (ROOT / "tests" / "issues" / "test_issue_3260.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3260.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3260.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3260.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3260-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    q = _read("src/compiler/evaluator_primitives_query_tail.cpp")
    if "schema-3260" in q or "schema-3260" in test:
        fails.append("AC5: new schema-3260 query key (SlimSurface)")

    if fails:
        print("FAIL #3260 macro_provenance_counter_unify:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3260 macro_provenance_counter_unify: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
