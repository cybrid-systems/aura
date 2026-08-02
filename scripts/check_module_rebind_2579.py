#!/usr/bin/env python3
"""Issue #2579: module rebind / multi-define value-init residual contracts.

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

    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    svc = _read("src/compiler/service.ixx")
    evalp = _read("src/compiler/evaluator_primitives_eval.cpp")
    eixx = _read("src/compiler/evaluator.ixx")
    test = _read("tests/compiler/test_module_rebind_residual_2579.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("#2579", "AC1", flat)
    must("any_non_lambda", "AC1", flat)
    must("#2579", "AC2", svc)
    must("sync_workspace_value_cells_from_env", "AC2", svc)
    must("do NOT bind_value_define_via_ir here", "AC2", svc)
    must("sync_workspace_value_cells_fn_", "AC3", eixx)
    must("sync_workspace_value_cells_fn_", "AC3", evalp)
    must("test_module_rebind_residual_2579", "AC4", cmake)
    must("check_module_rebind_2579", "AC4", build)
    must("ac1_multidefine_call_init", "AC4", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2579 module rebind residual — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
