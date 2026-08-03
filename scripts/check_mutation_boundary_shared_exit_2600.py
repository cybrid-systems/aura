#!/usr/bin/env python3
"""Issue #2600: shared exit helper for soft fiber boundary + full Guard
outermost success paths (refactor closes dual-rail drift).

Coverage gate: presence-checks for the shared exit helper
(src/compiler/mutation_boundary_shared_exit.h) + soft + full Guard
call sites + test additions + build.py wiring. Mirrors
`check_envframe_densify_scan_commit_barrier_2599.py` /
`check_panic_residual_densify_hard_2598.py` style.

Contract:
  AC6 mutation_boundary_shared_exit.h cites #2600 + declares helper +
      uses #2314 force_clear_residual_defer_for_evaluator + hold release +
      reconcile
  AC7 orch_soft_boundary_exit in evaluator_fiber_mutation.cpp calls
      mutation_boundary_shared_exit (AFTER mirror publish, BEFORE
      clearing g_orch_soft_boundary_ev)
  AC8 ResidualPolicy::Clear in evaluator_mutation_boundary.cpp calls
      mutation_boundary_shared_exit
  AC9 includes in both evaluator_fiber_mutation.cpp +
      evaluator_mutation_boundary.cpp
  AC10 build.py wires cmd_mutation_boundary_shared_exit_2600_coverage +
       gate script present

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

    hdr = _read("src/compiler/mutation_boundary_shared_exit.h")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    test = _read("tests/serve/test_orch_soft_boundary_unified_2515.cpp")
    build = _read("build.py")

    # AC6: header source-cite.
    must("Issue #2600", "AC6", hdr)
    must("mutation_boundary_shared_exit", "AC6", hdr)
    must("force_clear_residual_defer_for_evaluator", "AC6", hdr)
    must("mutation_hold_defer_active", "AC6", hdr)
    must("release_mutation_hold_defer", "AC6", hdr)
    must("reconcile_gc_defer_bits_after_clear", "AC6", hdr)
    must("Stack-light", "AC6", hdr)

    # AC7: soft path uses helper.
    must("orch_soft_boundary_exit", "AC7", efm)
    must("mutation_boundary_shared_exit", "AC7", efm)
    must("publish_mutation_safety_mirrors(depth, /*held=*/false", "AC7", efm)
    must("g_orch_soft_boundary_ev = nullptr;", "AC7", efm)

    # AC8: full Guard exit uses helper.
    must("ResidualPolicy::Clear", "AC8", emb)
    must("mutation_boundary_shared_exit", "AC8", emb)
    must("Issue #2600", "AC8", emb)

    # AC9: includes in both .cpp files.
    must(
        '#include "mutation_boundary_shared_exit.h"',
        "AC9",
        efm,
    ) if '#include "mutation_boundary_shared_exit.h"' in efm else must(
        "mutation_boundary_shared_exit.h",
        "AC9",
        efm,
    )
    must(
        '#include "mutation_boundary_shared_exit.h"',
        "AC9",
        emb,
    ) if '#include "mutation_boundary_shared_exit.h"' in emb else must(
        "mutation_boundary_shared_exit.h",
        "AC9",
        emb,
    )

    # AC10: test + build.py wiring.
    must("Issue #2600", "AC10", test)
    must("ac6_header_source_cite", "AC10", test)
    must("ac7_soft_path_uses_helper", "AC10", test)
    must("ac8_full_guard_uses_helper", "AC10", test)
    must("ac9_includes_source_cite", "AC10", test)
    must("ac10_build_gate_wiring_source_cite", "AC10", test)
    must("shared exit helper (soft fiber + full Guard)", "AC10", test)
    must("cmd_mutation_boundary_shared_exit_2600_coverage", "AC10", build)
    must("check_mutation_boundary_shared_exit_2600", "AC10", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(
            f"\n{len(fails)} mutation-boundary-shared-exit (#2600) contract row(s) failed",
            file=sys.stderr,
        )
        return 1
    print("OK: Issue #2600 mutation boundary shared exit — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
