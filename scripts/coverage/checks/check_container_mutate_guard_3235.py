#!/usr/bin/env python3
"""Issue #3235: vector-set! / hash-set! / set-car! acquire MutationBoundaryGuard.

mutate_general stamps requires_mutation_guard (#3197) but classic container
mutators were registered via register_prim, so PrimCall never acquired a
Guard. Production Restricted also denied kPrimSecSandboxed without
kCapSandbox. Result: CLI --load returned <error> / silent exit 0.

Contract:
  AC1 PrimCall auto-acquires Guard for requires_mutation_guard && !add_mutate
  AC2 vector-set! takes alloc_storage_lock_ (set-car! / hash-set! already do)
  AC3 production Restricted mutates values; try_acquire bumps
  AC4 source-cite; linter wired; no test_issue_*.cpp; no docs/design/

Exit 0 = all rows satisfied.
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

    eix = _read("src/compiler/evaluator.ixx")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    vec = _read("src/compiler/evaluator_primitives_vector.cpp")
    pair = _read("src/compiler/evaluator_primitives_pair.cpp")
    scaf = _read("src/compiler/prim_registrar_scaffold.hh")
    t = _read("tests/compiler/test_hash_table_grow.cpp")
    build = _read("build.py")

    must("maybe_auto_guard_heap_mutate", "AC1 PrimCall", eix)
    must("heap_mutate", "AC1 flag", eix)
    must("requires MutationBoundaryGuard", "AC1 error", eix)
    must("maybe_auto_guard_heap_mutate", "AC1 helper", mb)
    must("kContainerMutateGuardIssue = 3235", "AC1 stamp", scaf)
    must("3235 AC1: vector-ref 42", "AC1 test", t)

    must("alloc_storage_lock_", "AC2 vector-set!", vec)
    must("Issue #3235", "AC2 vector cite", vec)
    must("alloc_storage_lock_", "AC2 set-car!", pair)
    must("3235 AC2: hash-ref 4", "AC2 hash test", t)
    must("3235 AC2: car 99", "AC2 pair test", t)

    must("!heap_mutate", "AC3 skip sandbox cap", eix)
    must("3235 AC3: production vector-set!", "AC3 test", t)
    must("3235 AC3: try_acquire bumped", "AC3 metric", t)

    must("Issue #3235", "AC4 evaluator", eix)
    must("check_container_mutate_guard_3235", "AC4 build.py", build)
    must("3235 AC4: no invent", "AC4 test", t)
    if (ROOT / "tests" / "compiler" / "test_issue_3235.cpp").is_file():
        fails.append("AC4: tests/compiler/test_issue_3235.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3235.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3235.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3235-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3235 container_mutate_guard:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3235 container_mutate_guard: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
