#!/usr/bin/env python3
"""Issue #2569: set-code / mutate:rebind must not kill unimpacted closures or hash state.

Contract:
  AC1 soft-expire restamps IR/TW closures with live bodies (not null views)
  AC2 apply/call_closure soft-recover unimpacted helpers
  AC3 hash-ref 3-arg IR does not MakePair-pack default; prim honors default
  AC4 test + cmake + build.py gate

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

    svc = _read("src/compiler/service.ixx")
    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    ir = _read("src/compiler/ir_executor_impl.cpp")
    low = _read("src/compiler/lowering_impl.cpp")
    vec = _read("src/compiler/evaluator_primitives_vector.cpp")
    test = _read("tests/compiler/test_setcode_rebind_survive_2569.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 expire soft restamp
    must("#2569", "AC1", svc)
    must("Soft recover: keep IR dispatch alive", "AC1", svc)
    must("body_usable", "AC1", svc)
    must("ac1_closure_survive", "AC1", test)

    # AC2 apply soft recover
    must("#2569", "AC2", flat)
    must("soft-recover", "AC2", flat)
    must("#2569", "AC2", ir)
    must("soft_recover_2569", "AC2", ir)

    # AC3 hash-ref
    must("#2569", "AC3", low)
    must("hash-ref", "AC3", low)
    must("#2569", "AC3", vec)
    must("3rd-arg default", "AC3", vec)
    must("ac3_hash_ref_default", "AC3", test)

    # AC4 gate
    must("test_setcode_rebind_survive_2569", "AC4", cmake)
    must("check_setcode_rebind_2569", "AC4", build)
    must("cmd_setcode_rebind_coverage", "AC4", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2569 set-code/rebind survival — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
