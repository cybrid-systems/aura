#!/usr/bin/env python3
"""Issue #2554: promote chaos hard-fail counters into ./build.py gate.

Contract:
  AC1 inject hard-fail env + fail path in run_chaos_pass
  AC2 short PR profile (AURA_CHAOS_PR_GATE) with hard_fail_invariants
  AC3 FULL/SOAK paths retained (unchanged stricter nightly)
  AC4 build.py cmd_chaos_pr_hard_fail_gate + gate() registration
  AC5 hard-fail asserts present (steal hard-fail Δ, residual still-running)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    test = _read("tests/serve/test_chaos_mutate_steal_gc_mailbox_2352.cpp")
    build = _read("build.py")

    # AC1 inject
    must("AURA_CHAOS_PR_GATE_INJECT_HARD_FAIL", "AC1", test)
    must("bump_steal_snapshot_hard_fail", "AC1", test)
    must("AURA_CHAOS_PR_GATE_INJECT_HARD_FAIL", "AC1", build)

    # AC2 PR profile
    must("AURA_CHAOS_PR_GATE", "AC2", test)
    must("chaos_pr_gate", "AC2", test)
    must("hard_fail_invariants", "AC2", test)
    must("ac2554_pr_gate_short", "AC2", test)
    must("AURA_CHAOS_PR_GATE_ONLY", "AC2", test)

    # AC3 soak/full retained
    must("AURA_CHAOS_SOAK", "AC3", test)
    must("AURA_CHAOS_FULL", "AC3", test)
    must("if (!chaos_soak())", "AC3", test)
    must("if (!chaos_full())", "AC3", test)

    # AC4 gate wiring
    must("cmd_chaos_pr_hard_fail_gate", "AC4", build)
    must("cmd_chaos_pr_hard_fail_coverage", "AC4", build)
    must("check_chaos_pr_hard_fail_gate_2554", "AC4", build)
    must("AURA_CHAOS_PR_GATE", "AC4", build)
    # gate() chain includes the live PR chaos runner
    if "cmd_chaos_pr_hard_fail_gate()" not in build:
        fails.append("AC4: cmd_gate does not call cmd_chaos_pr_hard_fail_gate()")
    # CI static gate has no cmake tree — skip runtime when CMakeCache missing;
    # full runtime is in .github/workflows/ci.yml build-test after ./build.py ci.
    must("CMakeCache", "AC4", build)
    must("static coverage only", "AC4", build)
    ci = _read(".github/workflows/ci.yml")
    must("chaos-pr-hard-fail", "AC4", ci)
    must("Chaos PR hard-fail gate", "AC4", ci)

    # AC5 hard-fail asserts in gate path
    must("steal snapshot hard-fail delta == 0", "AC5", test)
    must("residual still-running gauge == 0", "AC5", test)
    must("deployment gate", "AC5", test)
    must("Issue #2554", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2554 chaos PR hard-fail gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
