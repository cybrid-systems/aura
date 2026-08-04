#!/usr/bin/env python3
"""Issue #2650 / #2649 H11: fiber-local eval recursion depth + H10 path refuse.

Contract:
  AC1 Fiber stores eval_c_stack_depth_ / env_lookup_depth_
  AC2 aura_eval_c_stack_depth_slot used by eval_flat
  AC3 env lookup uses aura_env_lookup_depth_slot
  AC4 load_module_file refuses non-module paths (#2653)
  AC5 unit test + cmake + build.py gate

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    fiber_h = _read("src/serve/fiber.h")
    fiber_c = _read("src/serve/fiber.cpp")
    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    env = _read("src/compiler/evaluator_env.cpp")
    loader = _read("src/compiler/evaluator_module_loader.cpp")
    test = _read("tests/compiler/test_fiber_eval_depth_isolation.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("eval_c_stack_depth_" in fiber_h, "AC1: Fiber.eval_c_stack_depth_")
    must("env_lookup_depth_" in fiber_h, "AC1: Fiber.env_lookup_depth_")
    must("aura_eval_c_stack_depth_slot" in fiber_h, "AC1: free slot API declared")
    must("#2650" in fiber_h or "#2649" in fiber_h, "AC1: fiber.h cites issue")

    must("aura_eval_c_stack_depth_slot" in fiber_c, "AC1: slot defined in fiber.cpp")
    must("aura_env_lookup_depth_slot" in fiber_c, "AC1: env slot defined")

    must("#2650" in flat, "AC2: eval_flat cites #2650")
    must("aura_eval_c_stack_depth_slot" in flat, "AC2: eval_flat uses fiber-local slot")
    must("fiber=" in flat, "AC2: depth error includes fiber id")

    must("aura_env_lookup_depth_slot" in env, "AC3: env uses fiber-local slot")
    must("#2650" in env or "#2649" in env, "AC3: env cites tracker")

    must("#2653" in loader, "AC4: loader cites #2653")
    must("is_plausible_module_path" in loader, "AC4: path validator")
    must("refuse empty path" in loader, "AC4: empty refuse message")

    must("AC1" in test and "AC4" in test, "AC5: unit test ACs")
    must(
        "test_fiber_eval_depth_isolation" in cmake,
        "AC5: cmake registers test target",
    )
    must("check_fiber_eval_depth_isolation_2650" in build, "AC5: build.py linter")
    must("cmd_fiber_eval_depth_isolation_coverage" in build, "AC5: coverage cmd")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2650 fiber-local eval depth + #2653 path refuse — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
