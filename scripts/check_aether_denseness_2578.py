#!/usr/bin/env python3
"""Issue #2578: Aether denseness host residual contracts (H1/H5/H6).

Contract:
  AC1 namespaced .aura-type parse (last ':' before ->)
  AC2 FuncType.variadic / dotted-rest call arity
  AC3 soft-recover module free-vars after unimpacted rebind
  AC4 materialize does not empty live module capture on bridge-stale
  AC5 test + cmake + build.py gate

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

    mod = _read("src/compiler/evaluator_module_loader.cpp")
    types = _read("src/compiler/evaluator_primitives_types.cpp")
    tc = _read("src/compiler/type_checker_impl.cpp")
    type_ixx = _read("src/core/type.ixx")
    env = _read("src/compiler/evaluator_env.cpp")
    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    aura_type = _read("lib/std/orchestrator.aura-type")
    test = _read("tests/compiler/test_aether_denseness_residual_2578.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 namespaced .aura-type
    must("#2578", "AC1", mod)
    must("rfind(':', arrow)", "AC1", mod)
    must("rfind(':', arrow)", "AC1", types)

    # AC2 variadic
    must("variadic", "AC2", type_ixx)
    must("#2578", "AC2", tc)
    must("ft.variadic", "AC2", tc)
    must("Any Any Any ...", "AC2", aura_type)

    # AC3 soft-recover free-vars
    must("#2578", "AC3", flat)
    must("apply_closure_must_deopt_soft_2581", "AC3", flat)
    must("soft_recover_2569", "AC3", flat)

    # AC4 materialize live body
    must("#2578", "AC4", env)
    must("#2581", "AC4", env)
    must("body_live", "AC4", env)

    # AC5 gate
    must("test_aether_denseness_residual_2578", "AC5", cmake)
    must("check_aether_denseness_2578", "AC5", build)
    must("cmd_aether_denseness_coverage", "AC5", build)
    must("ac2_orch_freevar_survive_rebind", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2578 Aether denseness residuals — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
