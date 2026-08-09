#!/usr/bin/env python3
"""Issue #2868: set-code/eval cross-pool SymId redefinition + module-frame bind.

Each (set-code …) allocates a fresh StringPool that reuses the same SymId
integer space. Without re-keying bindings_symid_ on Env::set_pool, later
isomorphic (define bbb …) reused aaa's cell via lookup_by_symid / multi-define
lookup_cell_index SymId paths — prior define body stolen, first multi-define
leaf string-unbound (module-frame unbound while top-level SymId collision
still "found" it). Unify residual: prom dual-leaf after aether install.

Contract (one row per AC):
  AC1 Env::set_pool re-keys bindings_symid_ on pool pointer change
  AC2 Define redef SymId path gated to same pool as AST
  AC3 multi-define Phase 1 reuses local string cells only (not lookup_cell_index)
  AC4 suite regression tests/suite/set_code_module_bind_2868.aura
  AC5 this linter wired in build.py; no docs/design/*

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

    env_cpp = _read("src/compiler/evaluator_env.cpp")
    env_ixx = _read("src/compiler/evaluator.ixx")
    efl = _read("src/compiler/evaluator_eval_flat.cpp")
    suite = _read("tests/suite/set_code_module_bind_2868.aura")
    build = _read("build.py")

    # AC1 — set_pool re-keys SymIds across pool changes.
    must("#2868", "AC1", env_cpp)
    must("void Env::set_pool", "AC1", env_cpp)
    must("bindings_symid_", "AC1", env_cpp)
    must("intern", "AC1", env_cpp)
    must("#2868", "AC1", env_ixx)
    must("void set_pool", "AC1", env_ixx)

    # AC2 — Define redef does not trust foreign-pool SymId equality.
    must("#2868", "AC2", efl)
    must("lookup_by_symid", "AC2", efl)
    must("eval_env.pool() == p", "AC2", efl)

    # AC3 — multi-define Phase 1 local string cell reuse only.
    must("#2868", "AC3", efl)
    must("local string", "AC3", efl)
    must("mutable_env.bindings()", "AC3", efl)
    # Must not *call* full lookup_cell_index in Phase 1 (comment may name it).
    anchor = efl.find("pre-allocate cells for all defines")
    if anchor < 0:
        fails.append("AC3: multi-define pre-allocate block missing")
    else:
        block = efl[anchor : anchor + 3500]
        phase2 = block.find("Phase 2:")
        phase1 = block[: phase2 if phase2 > 0 else len(block)]
        if "lookup_cell_index(" in phase1:
            fails.append("AC3: multi-define Phase 1 still calls lookup_cell_index (must be local string only — #2868)")
        must("existing_ci", "AC3", phase1)

    # AC4 — suite regression.
    must("2868", "AC4", suite)
    must("prom_leaf_a", "AC4", suite)
    must("run-2868!", "AC4", suite)
    must("OK-2868", "AC4", suite)

    # AC5 — linter wire + no docs.
    must("check_set_code_module_bind_2868", "AC5", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2868.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_2868.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2868-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2868 set-code module bind / cross-pool SymId — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
