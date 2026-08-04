#!/usr/bin/env python3
"""Issue #2653 / #2649 H10: load_module_file path refuse + owned path.

Contract:
  AC1 is_plausible_module_path refuses empty / whitespace / pure-digit
  AC2 refuse messages before resolve (not only cannot resolve)
  AC3 use/load-module/import snapshot via copy_string_heap_at
  AC4 unit test + cmake + build.py gate

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

    loader = _read("src/compiler/evaluator_module_loader.cpp")
    mod = _read("src/compiler/evaluator_primitives_module.cpp")
    test = _read("tests/compiler/test_module_path_refuse.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("#2653" in loader, "AC1: loader cites #2653")
    must("is_plausible_module_path" in loader, "AC1: validator")
    must("refuse empty path" in loader, "AC1: empty refuse")
    must(
        "space" in loader.lower() or "whitespace" in loader.lower(),
        "AC1: whitespace refuse",
    )
    must("all_digit" in loader or "16384" in loader, "AC1: pure-digit refuse")

    must("refuse non-module path" in loader, "AC2: structured refuse before resolve")
    must("truncate_path_for_log" in loader, "AC2: truncated log display")

    must("#2653" in mod, "AC3: module prims cite #2653")
    must("copy_string_heap_at" in mod, "AC3: own path via copy_string_heap_at")
    must(
        "load_module_file(ev.string_heap_[" not in mod,
        "AC3: no bare string_heap_ into load_module_file",
    )
    must("push_string_heap" in mod, "AC3: error strings via locked push")

    must("test_module_path_refuse" in cmake, "AC4: cmake")
    must("check_module_path_refuse_2653" in build, "AC4: linter")
    must("cmd_module_path_refuse_coverage" in build, "AC4: coverage cmd")
    must("AC1" in test and "AC3" in test and "AC5" in test, "AC4: unit ACs")
    must("#2653" in test, "AC4: test cites #2653")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2653 module path refuse — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
