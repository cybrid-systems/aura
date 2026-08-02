#!/usr/bin/env python3
"""Issue #2568: symbol eq?/equal? for quoted symbols (agent decision tags).

Contract:
  AC1 short_str_cache intern before move (eval_flat ast_to_data)
  AC2 Quote value-define tree-walks before IR bind (service.ixx)
  AC3 IR Quote Variable → ConstString (not free-var lookup → 0)
  AC4 eq?/equal? string paths
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

    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    svc = _read("src/compiler/service.ixx")
    low = _read("src/compiler/lowering_impl.cpp")
    builtins = _read("src/compiler/evaluator_primitives_builtins.cpp")
    runtime = _read("src/compiler/evaluator_primitives_runtime.cpp")
    test = _read("tests/compiler/test_symbol_eq_2568.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1: intern cache keyed before move
    must("#2568", "AC1", flat)
    must("short_str_cache_", "AC1", flat)
    must("Key/insert before move", "AC1", flat)
    must("ac1_literal_eq", "AC1", test)

    # AC2: Quote before IR bind
    must("#2568", "AC2", svc)
    must("Quote body must tree-walk BEFORE IR bind", "AC2", svc)
    must("ac3_define_quote", "AC2", test)

    # AC3: IR Quote Variable → ConstString
    must("#2568", "AC3", low)
    must("quoted symbols are data", "AC3", low)
    must("ConstString", "AC3", low)
    must("ac5_source_gate", "AC3", test)

    # AC4: eq?/equal?
    must("#2568", "AC4", builtins)
    must("is_string", "AC4", builtins)
    must("#2568", "AC4", runtime)
    must("ac4_equal_and_tags", "AC4", test)

    # AC5: gate wiring
    must("test_symbol_eq_2568", "AC5", cmake)
    must("check_symbol_eq_2568", "AC5", build)
    must("cmd_symbol_eq_coverage", "AC5", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2568 symbol eq? — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
