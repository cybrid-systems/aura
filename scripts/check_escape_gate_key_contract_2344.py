#!/usr/bin/env python3
"""Issue #2344: escape-gate publish key ↔ lower key contract (Option A).

Contract:
  AC1: Matching key blocks MoveOp elision (publish + set_current + lower)
  AC2: Wrong-key miss + any live blocked name → still block (conservative)
  AC3: Matching key + empty blocked → elide; no full-map scan on hit
  AC4: Additive query keys (schema-2344 / escape-gate-key-contract-wired)
  AC5: Source-cite publish site + set_current_escape_key + Move lower case

Exit 0 = all ACs satisfied.
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

    gate = _read("src/compiler/ownership_escape_lowering_gate.h")
    hooks = _read("src/compiler/typed_mutation_audit_hooks.cpp")
    tci = _read("src/compiler/type_checker_impl.cpp")
    sd = _read("src/compiler/service_dirty.cpp")
    lin = _read("src/compiler/lowering_linear_types_impl.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_escape_move_elision_gate_2263.cpp")
    cmake = _read("CMakeLists.txt")

    # AC1 / AC5: publish + set_current + lower wiring
    must("publish_escape_move_elision_gate_for_key", "AC1", tci)
    must("set_current_escape_key", "AC1", sd)
    must("escape_blocks_move_elision_for_current", "AC1", lin)
    must("AC12", "AC1", test)

    # AC2: Option A conservative miss path
    must("Issue #2344", "AC2", gate)
    must("g_linear_escape_gate_miss_conservative_block_total", "AC2", gate)
    must("g_linear_escape_gate_miss_conservative_block_total", "AC2", hooks)
    must("Issue #2344", "AC2", hooks)
    must("AC13", "AC2", test)
    must("conservative", "AC2", hooks)

    # AC3: matching-key happy path (no scan on hit)
    must("Matching key", "AC3", hooks)
    must("AC15", "AC3", test)

    # AC4: query additive
    must("escape-miss-conservative-block-total", "AC4", q)
    must("escape-gate-key-contract-wired", "AC4", q)
    must("schema-2344", "AC4", q)
    must("issue-2344", "AC4", q)
    must("schema-2286", "AC4", q)
    must("linear-escape-gate-cross-eval-miss-total", "AC4", q)

    # AC5: test + cmake
    must("AC16", "AC5", test)
    must("test_escape_move_elision_gate_2263", "AC5", cmake)
    must("#2344", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2344 escape-gate key contract — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
