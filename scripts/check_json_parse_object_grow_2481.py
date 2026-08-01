#!/usr/bin/env python3
"""Issue #2481: json-parse parse_object grows FlatHashTable (no silent drop).

Contract:
  AC1 grow_object_table + load factor 0.7
  AC2 fail-loud grow/insert PRIM_ERROR
  AC3 parse_object starts create(8) + #2481 cite
  AC4 json_object_key_hash helper
  AC5 gate wiring

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

    js = _read("src/compiler/evaluator_primitives_json.cpp")
    test = _read("tests/compiler/test_json_parse_object_grow_2481.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    pidx = js.find("auto parse_object")
    # grow + fail paths live deep in the lambda (~8k chars)
    body = js[pidx : pidx + 9000] if pidx >= 0 else ""

    must("Issue #2481", "AC1", js)
    must("grow_object_table", "AC1", body)
    must("size * 10", "AC1", body)
    must("capacity * 7", "AC1", body)

    must("hash table grow failed", "AC2", body)
    must("hash table insert failed", "AC2", body)
    must("make_primitive_error", "AC2", body)

    must("create(8)", "AC3", body)
    must("2481 AC1", "AC3", test)

    must("json_object_key_hash", "AC4", js)

    must("check_json_parse_object_grow_2481", "gate", build)
    must("cmd_json_parse_object_grow_coverage", "gate", build)
    must("test_json_parse_object_grow_2481", "gate", cmake)
    must("2481 AC5", "gate", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: json-parse object grow #2481 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
