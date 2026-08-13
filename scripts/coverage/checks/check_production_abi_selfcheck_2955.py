#!/usr/bin/env python3
"""Issue #2955: production startup strong-symbol ABI self-check.

Contract:
  AC1 production + missing strong → abort (main/self-check)
  AC2 Soft / sandbox=off → no forced abort
  AC3 full production link markers + ok path
  AC4 additive query keys; #2377 preserved
  AC5 source-cite + tests + build.py
  AC6 no docs/design; no invent test file
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

    hh = _read("src/serve/runtime_production_abi.h")
    cpp = _read("src/serve/runtime_production_abi.cpp")
    main_c = _read("src/main.cpp")
    fb = _read("src/compiler/fiber_bridge.cpp")
    fm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    fiber = _read("src/serve/fiber.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/serve/test_steal_complete_strong_entry.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2955", "AC1", hh)
    must("aura_runtime_require_production_abi", "AC1", hh)
    must("std::abort()", "AC1", cpp)
    must("aura_runtime_require_production_abi", "AC1", main_c)
    must("apply_production_security_defaults", "AC1", main_c)
    dpos = main_c.find("apply_production_security_defaults")
    cpos = main_c.find("aura_runtime_require_production_abi")
    if dpos < 0 or cpos < 0 or dpos > cpos:
        fails.append("AC1: main must call self-check after production defaults")
    must("2955 AC1", "AC1", test)

    # AC2
    must("sandbox_is_off", "AC2", cpp)
    must("production_abi_selfcheck_required", "AC2", cpp)
    must("AURA_SANDBOX", "AC2", cpp)
    must("2955 AC2", "AC2", test)

    # AC3 strong markers
    must("aura_abi_strong_steal_complete_v", "AC3", fm)
    must("aura_abi_strong_mutation_held_v", "AC3", fm)
    must("aura_abi_strong_mutation_depth_from_ptr_v", "AC3", fm)
    must("aura_abi_strong_fiber_eval_id_v", "AC3", fiber)
    must("aura_abi_strong_steal_complete_v", "AC3", fb)  # weak returns 0
    must("return 0", "AC3", fb)
    must("2955 AC3", "AC3", test)

    # AC4
    must("schema-2955", "AC4", q)
    must("production-abi-selfcheck-wired", "AC4", q)
    must("production-abi-selfcheck-ok-total", "AC4", q)
    must("production-abi-selfcheck-fail-total", "AC4", q)
    must("schema-2377", "AC4", q)

    # AC5 / AC6
    must("runtime_production_abi.cpp", "AC5", cmake)
    must("check_production_abi_selfcheck_2955", "AC5", build)
    must("2955", "AC5", test)
    if (ROOT / "tests" / "serve" / "test_issue_2955.cpp").is_file():
        fails.append("AC6: test_issue_2955.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2955-*"):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2955 production ABI self-check")
    return 0


if __name__ == "__main__":
    sys.exit(main())
