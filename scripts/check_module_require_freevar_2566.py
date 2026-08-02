#!/usr/bin/env python3
"""Issue #2566: non-std module free-var resolve of required std bindings.

Contract:
  AC1 nested require injects into module env (RequireInjectGuard)
  AC2 import uses require_inject_env_ (not top_-only)
  AC3 SoA live top_ fallback when walk reaches root frame
  AC4 source-cite #2566
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

    loader = _read("src/compiler/evaluator_module_loader.cpp")
    prim = _read("src/compiler/evaluator_primitives_module.cpp")
    env = _read("src/compiler/evaluator_env.cpp")
    eixx = _read("src/compiler/evaluator.ixx")
    test = _read("tests/compiler/test_module_require_freevar_2566.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("RequireInjectGuard", "AC1", loader)
    must("require_inject_env_", "AC1", loader)
    must("ac1_module_freevar_parity", "AC1", test)

    # AC2
    must("require_inject_env_", "AC2", prim)
    must("require_inject_env_", "AC2", eixx)
    must("ac2_toplevel_inject", "AC2", test)

    # AC3
    must("#2566", "AC3", env)
    must("cur == 0", "AC3", env)
    must("ac4_soa_live_top", "AC3", test)

    # AC4
    must("#2566", "AC4", loader)
    must("#2566", "AC4", prim)
    must("ac3_source_cite_inject", "AC4", test)

    # AC5
    must("test_module_require_freevar_2566", "AC5", cmake)
    must("check_module_require_freevar_2566", "AC5", build)
    must("cmd_module_require_freevar_coverage", "AC5", build)
    must("ac5_gate", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2566 module free-var require inject — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
