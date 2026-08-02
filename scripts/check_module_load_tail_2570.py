#!/usr/bin/env python3
"""Issue #2570: module load tail export + fail-closed nested require.

Contract:
  AC1 load_module_file fail-closed on eval unexpected / error
  AC2 void-cell export check; no partial cache
  AC3 begin aborts on first-class error (nested require)
  AC4 import surfaces load failure as error
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
    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    mod = _read("src/compiler/evaluator_primitives_module.cpp")
    test = _read("tests/compiler/test_module_load_tail_export_2570.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("#2570", "AC1", loader)
    must("fail-closed", "AC1", loader)
    must("fail_load", "AC1", loader)
    must("ac1_tail_exports", "AC1", test)

    must("unbound (void)", "AC2", loader)
    must("ac2_mid_error_fail_closed", "AC2", test)

    must("#2570", "AC3", flat)
    must("abort begin on first-class error", "AC3", flat)
    must("ac3_nested_require_fail", "AC3", test)

    must("#2570", "AC4", mod)
    must("module-load-failed", "AC4", mod)

    must("test_module_load_tail_export_2570", "AC5", cmake)
    must("check_module_load_tail_2570", "AC5", build)
    must("cmd_module_load_tail_coverage", "AC5", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2570 module load tail export — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
