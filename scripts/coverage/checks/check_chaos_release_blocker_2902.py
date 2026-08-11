#!/usr/bin/env python3
"""Issue #2902: elevate production chaos gate to hard release blocker.

Contract:
  AC1 clean run under production_defaults + ≥32 fibers; expanded hard-fail set
  AC2 known-bad inject fails under production; Soft forbid under hard gates
  AC3 sustained mode (AURA_CHAOS_SUSTAINED) documented + seed/iters
  AC4 structural source-cite (function/env names, not line numbers)
  AC5 build.py release blocker + linter; no docs/design/*
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

    chaos = _read("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp")
    build = _read("build.py")

    must("AURA_CHAOS_RELEASE_BLOCKER", "AC1", chaos)
    must("apply_production_audit_defaults", "AC1", chaos)
    must("residual_rearm_race", "AC1", chaos)
    must("residual_defer_steal_hard_fail", "AC1", chaos)
    must("resume_fence", "AC1", chaos)
    must("d_resume_fence_hard", "AC1", chaos)
    must("fibers >= 32", "AC1", chaos)
    must("composition-aware ceiling", "AC1", chaos)

    must("AURA_CHAOS_PR_GATE_INJECT_HARD_FAIL", "AC2", chaos)
    must("bump_steal_snapshot_hard_fail", "AC2", chaos)
    must("Soft steal forbidden", "AC2", chaos)

    must("AURA_CHAOS_SUSTAINED", "AC3", chaos)
    must("chaos_sustained", "AC3", chaos)
    must("AURA_CHAOS_SEED", "AC3", chaos)

    must("run_chaos_pass", "AC4", chaos)
    must("hard_fail_invariants", "AC4", chaos)
    must("ac2554_pr_gate_short", "AC4", chaos)
    must("ac2856_1_production_chaos_gate_runnable", "AC4", chaos)
    must("ac2755_1_residual_zero_under_hard_gate", "AC4", chaos)

    must("ac2902_1_clean_run_zero_hard_fail", "AC5", chaos)
    must("ac2902_2_inject_fails_production", "AC5", chaos)
    must("ac2902_3_sustained_mode", "AC5", chaos)
    must("ac2902_4_structural_source_cite", "AC5", chaos)
    must("ac2902_5_release_blocker_docs_and_linter", "AC5", chaos)
    must("check_chaos_release_blocker_2902", "AC5", build)
    must("cmd_chaos_release_blocker_2902", "AC5", build)
    must("AURA_CHAOS_RELEASE_BLOCKER", "AC5", build)
    must("chaos-release-blocker-2902", "AC5", build)

    # Hard-fail set documentation present in source.
    for counter in (
        "steal_snapshot_hard_fail",
        "residual_rearm_race",
        "residual_defer_steal_hard_fail",
        "resume_fence hard",
    ):
        must(counter, "AC1-set", chaos)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2902-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "serve" / "test_issue_2902.cpp").is_file():
        fails.append("tests/serve/test_issue_2902.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2902 chaos hard release blocker — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
