#!/usr/bin/env python3
"""Issue #2571: (define) inside (while) body loop-counter footgun.

Contract:
  AC1 lookup_cell_index / lookup_cell_ptr newest cell (#2571)
  AC2 multi-define begin reuses existing cells in while
  AC3 education warning on define-in-while special form
  AC4 language note on while primitive
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

    env = _read("src/compiler/evaluator_env.cpp")
    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    ctrl = _read("src/compiler/evaluator_primitives_control.cpp")
    test = _read("tests/compiler/test_while_define_oneshot_2571.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("#2571", "AC1", env)
    must("lookup_cell_index", "AC1", env)
    must("binding_index_", "AC1", env)
    must("ac1_define_in_while", "AC1", test)

    must("reuse an existing cell", "AC2", flat)
    must("ac2_multi_define_in_while", "AC2", test)

    must("#2571", "AC3", flat)
    must("define …) inside (while", "AC3", flat)
    must("ac3_preferred_outer_set", "AC3", test)

    must("#2571", "AC4", ctrl)
    must("outer-define", "AC4", ctrl)
    must("set! x 0", "AC4", ctrl)

    must("test_while_define_oneshot_2571", "AC5", cmake)
    must("check_while_define_oneshot_2571", "AC5", build)
    must("cmd_while_define_oneshot_coverage", "AC5", build)
    must("ac4_source_gate", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2571 while+define oneshot — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
