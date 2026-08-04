#!/usr/bin/env python3
"""Issue #2417: binding_gens_ atomic shared_ptr + COW bump.

Contract:
  AC1 atomic<shared_ptr<BindingGenMap>> declaration
  AC2 binding_gen loads snapshot; bump COW+CAS
  AC3 compact/clone store fresh map
  AC4 no in-place gens[sym]++ without COW

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

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    ast = _read("src/core/ast.ixx")
    test = _read("tests/core/test_binding_gens_atomic.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2417", "AC1", ast)
    must("std::atomic<std::shared_ptr<BindingGenMap>>", "AC1", ast)
    must_not(
        "std::shared_ptr<BindingGenMap> binding_gens_ = std::make_shared<BindingGenMap>();",
        "AC1",
        ast,
    )
    must("2417 AC1", "AC1", test)

    must("binding_gens_.load(std::memory_order_acquire)", "AC2", ast)
    must("compare_exchange_weak", "AC2", ast)
    must("2417 AC2", "AC2", test)

    must("binding_gens_.store(std::make_shared<BindingGenMap>()", "AC3", ast)
    must("2417 AC3", "AC3", test)

    # In-place mutate on shared map without COW is gone
    must_not("binding_gens_->gens[sym]++", "AC4", ast)
    must("2417 AC4", "AC4", test)

    must("check_binding_gens_atomic_2417", "gate", build)
    must("cmd_binding_gens_atomic_coverage", "gate", build)
    must("test_binding_gens_atomic", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: binding_gens atomic #2417 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
