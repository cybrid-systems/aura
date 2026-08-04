#!/usr/bin/env python3
"""Issue #2550: named closure create forces stable_func_id != 0.

Contract:
  AC1 set_name uses get_or_preserve for non-empty names; anonymous stays 0
  AC2 no residual lookup-only stamp line on named set_name
  AC3 getter + residual force helpers declared; backfill remains safety net
  AC4 schema-2550 + wired key on query surface
  AC5 test + cmake + build.py gate

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
            fails.append(f"{label}: residual forbidden {n!r}")

    rt = _read("src/compiler/aura_jit_runtime.cpp")
    bh = _read("src/compiler/aura_jit_bridge.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_named_closure_stable_id_at_create.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2550", "AC1", rt)
    must("void aura_closure_set_name", "AC1", rt)
    must("aura_get_or_preserve_stable_func_id(name, &preserved)", "AC1", rt)
    must("never leave 0 for named", "AC1", rt)
    must("anonymous by design", "AC1", rt)

    # AC2: no old lookup-only named stamp
    must_not("name ? aura_lookup_stable_func_id(name) : 0", "AC2", rt)

    # AC3
    must("aura_get_closure_stable_func_id", "AC3", bh + rt)
    must("aura_test_force_closure_stable_func_id", "AC3", bh + rt)
    must("aura_bump_live_closure_stable_id_backfill_total", "AC3", rt)
    must("residual named sid==0", "AC3", rt)

    # AC4 query
    must("schema-2550", "AC4", q)
    must("issue-2550", "AC4", q)
    must("named-closure-stable-id-at-create-wired", "AC4", q)

    # AC5 wiring
    must("ac1_named_create_nonzero", "AC5", test)
    must("ac2_reemit_no_backfill_growth", "AC5", test)
    must("test_named_closure_stable_id_at_create", "AC5", cmake)
    must("check_named_closure_stable_id_at_create_2550", "AC5", build)
    must("cmd_named_closure_stable_id_at_create_coverage", "AC5", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2550 named closure stable_id at create — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
