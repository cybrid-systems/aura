#!/usr/bin/env python3
"""Issue #3085: densify/steal rehydrate-miss blocks lowering elision.

#3032 / #3063 already advance invalidate_gen. Lowering Move elision
must consult that gen (via depth_or_densify_block) so a still-green
proof stamp cannot elide between the miss and the next outermost
restamp. Abort still uses clear_type_linear_commit_proof_on_abort.
Soft / no densify: inv==0, no extra gen load.

Contract (one row per AC):
  AC1 miss advances gen; lowering helper blocks elision
  AC2 linear_fast_path_ok false until green rebind
  AC3 abort clear unchanged (no double-clear)
  AC4 Soft / inv==0 → no block
  AC5 extend persist-rehydrate + escape-elision; this linter; no invent

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    tma = _read("src/compiler/typed_mutation_audit.h")
    hooks = _read("src/compiler/typed_mutation_audit_hooks.cpp")
    low = _read("src/compiler/lowering_linear_types_impl.cpp")
    gate = _read("src/compiler/ownership_escape_lowering_gate.h")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    persist = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    esc = _read("tests/compiler/test_escape_move_elision_gate.cpp")
    q = read_query_prims()
    build = _read("build.py")

    must("kLinearFastPathRehydrateGenElisionIssue = 3085", "AC1 stamp", tma)
    must("linear_fast_path_rehydrate_gen_blocks_elision", "AC1 helper", tma)
    must("linear_fast_path_rehydrate_gen_blocks_elision", "AC1 hooks", hooks)
    must("Issue #3085", "AC1 hooks cite", hooks)
    must("Issue #3085", "AC1 lowering", low)
    must("aura_linear_fast_path_depth_or_densify_block", "AC1 lowering helper", low)
    must("ac3085_1_densify_miss_blocks_elision", "AC1 test", persist)
    must("3085 AC1: lowering helper blocks after miss", "AC1 soak", persist)

    must("linear_fast_path_ok", "AC2 SSOT", tma)
    must("ac3085_2_green_rebind_restores", "AC2 test", persist)
    must("3085 AC2: green rebind restores ok", "AC2 restore", persist)

    must("clear_type_linear_commit_proof_on_abort", "AC3 abort", tma)
    must("clear_type_linear_commit_proof_on_abort", "AC3 dtor", mb)
    must("ac3085_3_abort_clear_unchanged", "AC3 test", persist)
    if "clear_type_linear_commit_proof_on_abort" in hooks and hooks.count(
        "clear_type_linear_commit_proof_on_abort"
    ) > mb.count("clear_type_linear_commit_proof_on_abort"):
        fails.append("AC3: extra abort-clear (must reuse existing helper)")

    must("if (inv == 0)", "AC4 early-out", tma)
    must("ac3085_4_soft_zero_extra", "AC4 test", persist)
    must("3085 AC4: no gen bump", "AC4 gen", persist)

    must_key("schema-3085", "AC5 schema", q)
    must_key("linear-fast-path-rehydrate-gen-elision-wired", "AC5 wired", q)
    must("schema-3032", "AC5 lineage 3032", q)
    must("schema-3063", "AC5 lineage 3063", q)
    must("ac3085_5_schema_and_linter", "AC5 test", persist)
    must("ac3085_hermetic_lowering_block", "AC5 escape", esc)
    must("check_rehydrate_miss_lowering_elision_3085", "AC5 build.py", build)
    must("Issue #3085", "AC5 gate header", gate)
    if (ROOT / "tests" / "compiler" / "test_issue_3085.cpp").is_file():
        fails.append("AC5: test_issue_3085.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3085-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3085 rehydrate-miss lowering elision — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
