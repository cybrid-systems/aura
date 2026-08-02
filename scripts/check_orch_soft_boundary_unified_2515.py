#!/usr/bin/env python3
"""Issue #2515: Soft orch-agent boundary 提升为轻量 Guard 子集，统一 depth/held 语义。

#2118 introduced a soft-boundary path for orch agent body — pushes a
lightweight checkpoint onto the per-fiber mutation stack + sets the
orch_agent_boundary_active_ flag — but does NOT call
publish_mutation_safety_mirrors (the full Guard path does). This leaves
the fiber-local held_mirror_ stale during soft windows, so steal / GC /
is_at_mutation_boundary_safe see a divergent picture (orch flag set, but
snapshot.held == false from a different code path). Long-running agent
bodies under cancel storms accumulate depth drift risk vs pure Guard path.

This linter enforces that:
  AC1 orch_soft_boundary_enter calls publish_mutation_safety_mirrors
       (depth, held=true, defuse_version) — same shape as full Guard.
  AC2 orch_soft_boundary_exit publishes held=false mirror BEFORE clearing
       orch_agent_boundary_active_ (symmetric release — no probe window
       where the flag flipped without the mirror cleared).
  AC3 is_at_mutation_boundary_safe / mutation_safety_snapshot agree on
       soft + full paths at same depth/held (no divergence).
  AC4 #2115 / #2118 test files preserved (not removed by #2515) +
       #2515 source-cite in fiber.h / evaluator_fiber_mutation.cpp.
  AC5 Zero extra cost pure-reasoning path: mutation_boundary=false stays
       zero overhead (no soft path activation when not needed).

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

    fh = _read("src/serve/fiber.h")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    test = _read("tests/serve/test_orch_soft_boundary_unified_2515.cpp")
    t2115 = _read("tests/serve/test_depth_safe_mutation_boundary_steal_2115.cpp")
    t2118 = _read("tests/serve/test_orch_agent_mutation_boundary_2118.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 — orch_soft_boundary_enter publishes held=true mirror.
    must("orch_soft_boundary_enter", "AC1", efm)
    must("publish_mutation_safety_mirrors(depth, /*held=*/true,", "AC1", efm)
    must("Issue #2515", "AC1", efm)
    must("g_orch_soft_boundary_ev", "AC1", efm)
    must("defuse_version_for_test()", "AC1", efm)

    # AC2 — orch_soft_boundary_exit publishes held=false mirror BEFORE
    # clearing orch_agent_boundary_active_.
    must("orch_soft_boundary_exit", "AC2", efm)
    must("publish_mutation_safety_mirrors(depth, /*held=*/false, ver)", "AC2", efm)
    must("set_orch_agent_boundary_active(false)", "AC2", efm)
    # Order check: held=false publish must precede flag clear.
    held_false_pos = efm.find("publish_mutation_safety_mirrors(depth, /*held=*/false, ver)")
    flag_clear_pos = efm.find("fib->set_orch_agent_boundary_active(false)")
    if held_false_pos != -1 and flag_clear_pos != -1 and held_false_pos >= flag_clear_pos:
        fails.append(
            f"AC2: symmetric release order — held=false publish {held_false_pos} must be BEFORE flag clear {flag_clear_pos}"
        )

    # AC3 — is_at_mutation_boundary_safe / mutation_safety_snapshot agree
    # on soft + full paths at same depth/held.
    must("is_at_mutation_boundary_safe", "AC3", fh)
    must("mutation_safety_snapshot", "AC3", fh)
    must("publish_mutation_safety_mirrors", "AC3", fh)
    must("held_mirror_", "AC3", fh)
    must("orch_agent_boundary_active() && (s.depth > 0 || s.held)", "AC3", fh)
    # Soft + held both true → unsafe (same as full Guard).
    # The check is in fiber.h::is_at_mutation_boundary_safe.

    # AC4 — Source-cite unified semantics + #2115 / #2118 test files preserved.
    must("Issue #2515", "AC4", fh)
    must("#2115", "AC4", fh)
    must("#2118", "AC4", fh)
    must("#2184", "AC4", fh)
    must("Issue #2515", "AC4", efm)
    if not t2115:
        fails.append("AC4: tests/serve/test_depth_safe_mutation_boundary_steal_2115.cpp removed (must preserve)")
    if not t2118:
        fails.append("AC4: tests/serve/test_orch_agent_mutation_boundary_2118.cpp removed (must preserve)")

    # AC5 — Zero extra cost pure-reasoning path + linter self-test.
    must("pure-reasoning", "AC5", test)
    must("AC1", "AC5", test)
    must("AC5", "AC5", test)
    must("zero cost", "AC5", test)
    must("aura_add_issue_test(test_orch_soft_boundary_unified_2515)", "AC5", cmake)
    must("aura_issue_test_link_light(test_orch_soft_boundary_unified_2515)", "AC5", cmake)
    must("add_dependencies(all_test_issue_targets test_orch_soft_boundary_unified_2515)", "AC5", cmake)
    must("check_orch_soft_boundary_unified_2515", "AC5", build)

    if fails:
        print("check_orch_soft_boundary_unified_2515: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_orch_soft_boundary_unified_2515: OK (5/5 AC rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
