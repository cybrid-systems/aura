#!/usr/bin/env python3
"""Issue #2956: outermost Guard / soft-boundary post-publish mirror canary.

Contract:
  AC1 outermost enter/exit publish then canary; production hard path records
  AC2 nested Guard does not pay canary (is_active=0)
  AC3 Soft metric-only; production defaults hard canary wired
  AC4 steal still has independent snapshot check (defense in depth)
  AC5 source-cite + tests + linter; no docs/design; no invent test file
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

    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    fh = _read("src/serve/fiber.h")
    fb = _read("src/compiler/fiber_bridge.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    wc = _read("src/serve/worker.cpp")
    test = _read("tests/serve/test_mutation_safety_snapshot_steal.cpp")
    build = _read("build.py")

    # AC1 — outermost Guard enter/exit call canary after publish
    must("Issue #2956", "AC1", emb)
    must("aura_mutation_boundary_assert_mirrors_consistent", "AC1", emb)
    must("publish_current_fiber_mutation_safety", "AC1", emb)
    # Both enter (expect_held=1) and exit (expect_held=0) sites
    if emb.count("aura_mutation_boundary_assert_mirrors_consistent") < 2:
        fails.append("AC1: Guard TU must call canary at enter and exit (≥2)")

    # Soft orch publish sites also canary
    must("aura_mutation_boundary_assert_mirrors_consistent", "AC1", efm)
    must("orch_soft_boundary_enter", "AC1", efm)
    must("orch_soft_boundary_exit", "AC1", efm)
    if efm.count("aura_mutation_boundary_assert_mirrors_consistent") < 3:
        # helper def + soft enter + soft exit
        fails.append("AC1: fiber_mutation must define helper + soft enter/exit canary (≥3)")

    # Helper + counters
    must("g_mutation_mirror_inconsistency_hard_total", "AC1", efm)
    must("g_mutation_mirror_inconsistency_soft_total", "AC1", efm)
    must("is_steal_snapshot_hard_mode", "AC1", efm)
    must("aura_evaluator_mark_outermost_mutation_failed", "AC1", efm)

    # AC2 — nested skip documented
    must("is_active", "AC2", fh)
    must("is_active == 0", "AC2", efm)
    must("Nested Guard", "AC2", efm)

    # AC3 — Soft vs hard counters + production hard mode reuse
    must("mutation-mirror-inconsistency-hard-total", "AC3", q)
    must("mutation-mirror-inconsistency-soft-total", "AC3", q)
    must("schema-2956", "AC3", q)
    must("mutation-mirror-canary-wired", "AC3", q)

    # AC4 — steal independent check retained
    must("mutation_safety_snapshot_inconsistent", "AC4", wc)

    # AC5 — tests + weak stubs + build
    must("2956", "AC5", test)
    must("aura_mutation_boundary_assert_mirrors_consistent", "AC5", test)
    must("schema-2956", "AC5", test)
    must("aura_mutation_boundary_assert_mirrors_consistent", "AC5", fb)
    must("__attribute__((weak))", "AC5", fb)
    must("check_mutation_mirror_canary_2956", "AC5", build)
    if (ROOT / "tests" / "serve" / "test_issue_2956.cpp").is_file():
        fails.append("AC5: test_issue_2956.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2956-*"):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2956 mutation mirror canary")
    return 0


if __name__ == "__main__":
    sys.exit(main())
