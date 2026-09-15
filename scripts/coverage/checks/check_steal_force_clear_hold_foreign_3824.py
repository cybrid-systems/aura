#!/usr/bin/env python3
"""Issue #3824: force_clear residual must not release live MutationHold.

Steal-complete / force_clear_residual_defer_for_evaluator always released
process-wide MutationHold when active, even when another evaluator still
holds outermost Guard. EnvFrame/Panic stay identity-keyed; MutationHold
release is refused while process held count > 0.

Contract (one row per AC):
  AC1  force_clear refuses hold release while live Guard held
  AC2  orphan residual (no live Guard) still clears; B exit → clear
  AC3  steal-complete cites #3824; shared_exit refuses foreign release
  AC4  Soft leftover path unchanged; tests extend steal_complete_gc_defer

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

    gh = _read("src/core/gc_hooks.h")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    shared = _read("src/compiler/mutation_boundary_shared_exit.h")
    t = _read("tests/serve/test_steal_complete_gc_defer.cpp")
    build = _read("build.py")

    must("Issue #3824", "AC1 cite", gh)
    must("mutation_hold_live_guard_held_for_force_clear", "AC1 helper", gh)
    must("aura_process_mutation_boundary_held_count", "AC1 held SSOT", gh)

    # AC1: refuse path next to force_clear hold release
    fc = gh.find("force_clear_residual_defer_for_evaluator")
    if fc < 0:
        fails.append("AC1: force_clear_residual_defer_for_evaluator missing")
    else:
        # Prefer the definition body (second occurrence after earlier mentions).
        fc2 = gh.find("inline ResidualClearResult force_clear_residual_defer_for_evaluator")
        win = gh[fc2 if fc2 >= 0 else fc : (fc2 if fc2 >= 0 else fc) + 1800]
        must("mutation_hold_live_guard_held_for_force_clear", "AC1 refuse gate", win)
        must("release_mutation_hold_defer", "AC1 orphan release", win)
        must("Issue #3824", "AC1 body cite", win)

    must("Issue #3824", "AC3 steal cite", efm)
    must("Issue #3824", "AC3 shared_exit cite", shared)
    must("mutation_hold_live_guard_held_for_force_clear", "AC3 shared_exit gate", shared)

    must("3824 AC1: mutation_hold_defer_active still true while B.held", "AC2 AC1 test", t)
    must("3824 AC2: orphan force_clear released hold", "AC2 orphan test", t)
    must("3824 AC1: no destructive GC admit mid-B-hold (chaos)", "AC3 chaos test", t)
    must("3824 AC4: Soft leftover residual path unchanged", "AC4 Soft test", t)
    must("check_steal_force_clear_hold_foreign_3824", "AC4 build.py", build)

    if (ROOT / "tests" / "serve" / "test_issue_3824.cpp").is_file():
        fails.append("AC4: test_issue_3824.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3824.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3824.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3824-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print(f"Issue #3824 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3824 steal force_clear hold foreign refuse — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
