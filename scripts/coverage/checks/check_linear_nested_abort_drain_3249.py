#!/usr/bin/env python3
"""Issue #3249: nested Guard abort drains leftover linear_roots.

#3023 drains on outermost fail + post-join. Nested abort skipped that
path, so nested fail + outer success left sticky pins. Production nested
abort now drains extras vs enter snapshot; outer pins stay. Steal
hard-fail shares unpin_all with post-join. Densify still never unpins.

Contract (one row per AC):
  AC1  nested abort drains extras; outer pins remain
  AC2  abort/fail share unpin_all / unpin_linear_roots_except
  AC3  densify verify never unpins; leftover drained on abort
  AC4  soak nested abort; abort-release total; live_count quiescent
  AC5  Soft nested abort does not drain; no second pin model; no invent

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

    lp = _read("src/core/lifetime_pin.hh")
    ixx = _read("src/core/lifetime_pin.ixx")
    bnd = _read("src/compiler/evaluator_mutation_boundary.cpp")
    fibm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    fib = _read("src/serve/fiber.cpp")
    test = _read("tests/compiler/test_linear_pin_moving_compact.cpp")
    build = _read("build.py")

    must("kLinearNestedAbortDrainIssue = 3249", "AC1 stamp", lp)
    must("snapshot_linear_roots", "AC1 snapshot", lp)
    must("unpin_linear_roots_except", "AC1 except-keep", lp)
    must("unpin_linear_roots_except", "AC1 nested dtor", bnd)
    must("nested_linear_keep_armed_", "AC1 nested snapshot arm", bnd)
    must("production_defaults_active()", "AC5 Soft skip snapshot", bnd)

    must("unpin_all_linear_roots", "AC2 outermost still unpin_all", _read("src/compiler/evaluator_gc.cpp"))
    must("unpin_all_linear_roots", "AC2 post-join", fib)
    must("unpin_all_linear_roots", "AC2 steal hard-fail", fibm)
    must("Issue #3249", "AC2 steal cite", fibm)

    must("this verify never unpins", "AC3 densify never unpins", lp)
    must("ac3249_3_densify", "AC3 densify canary", test)

    must("ac3249_1_nested", "AC1 nested test", test)
    must("ac3249_4_soak", "AC4 soak", test)
    must("ac3249_5_soft", "AC5 Soft test", test)
    must("unpin_linear_roots_except", "AC2 ixx export", ixx)
    must("check_linear_nested_abort_drain_3249", "AC5 build.py", build)
    must("cmd_linear_nested_abort_drain_3249_coverage", "AC5 cmd", build)
    if "AgentRegistry" in lp[lp.find("Issue #3249") : lp.find("Issue #3249") + 1800]:
        fails.append("AC5: must not introduce AgentRegistry")
    if _read("tests/compiler/test_issue_3249.cpp") or _read("tests/core/test_issue_3249.cpp"):
        fails.append("AC5: test_issue_3249.cpp present (forbidden #81967)")
    if _read("docs/design/3249-nested-abort-linear-drain.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3249 linear_nested_abort_drain:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3249 linear_nested_abort_drain: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
