#!/usr/bin/env python3
# scripts/check_expand_checkpoint_local_rollback_3608.py -- Issue #3608 source-cite gate.
#
# AC1: production clone/expand deny without a current Evaluator leaves the
#      target FlatAST unchanged — the guard's local snapshot (node count at
#      install) + FlatAST::truncate_to truncate half-adds on deny.
# AC2: gensym-ceiling / steal-abort deny sites share the same restore path
#      (try_restore fallback) — reason codes 1 and 6 keep their stamps.
# AC3: the owned #3062 path is preserved — with a live Evaluator the C-ABI
#      save/restore/commit panic-checkpoint brick still runs first.
# AC4: MutationBoundary stays SSOT — install returns 0 under an active
#      boundary and the local snapshot is NOT armed (no double restore).
# AC5: Soft/Off — no save, no snapshot, no extra walk (production gates).
# AC6: build.py wires this linter; ac3608_* rows live in the hygiene limits
#      suite; no docs/design/*3608*, no tests/**/test_issue_3608.cpp.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

MX = "src/compiler/macro_expansion.cpp"
AST = "src/core/ast.ixx"
FIB = "src/compiler/evaluator_fiber_mutation.cpp"
TEST = "tests/compiler/test_macro_hygiene_limits.cpp"
BUILD = "build.py"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    _ = ap.parse_args()

    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    mx = _read(MX)
    ast = _read(AST)
    fib = _read(FIB)
    test = _read(TEST)
    build = _read(BUILD)

    # AC1
    must("Issue #3608", "AC1 macro_expansion", mx)
    must("install_local_snapshot", "AC1 guard arm", mx)
    must("truncate_to", "AC1 truncate wired", mx)
    must("Issue #3608", "AC1 ast.ixx", ast)
    must("void truncate_to(std::size_t keep_size)", "AC1 primitive", ast)
    must("ac3608_1_depth_no_evaluator_size_unchanged", "AC1 test row", test)

    # AC2
    must("ac3608_2_gensym_no_evaluator_size_unchanged", "AC2 test row", test)
    must("kHygieneLimitReasonStealAbort", "AC2 steal reason exists", mx)
    # AC3
    must("aura_evaluator_try_save_macro_expand_checkpoint", "AC3 save ABI", fib)
    must("aura_evaluator_try_restore_macro_expand_checkpoint", "AC3 restore ABI", mx)
    must("aura_evaluator_commit_macro_expand_checkpoint", "AC3 commit ABI", fib)
    must("if (owned)", "AC3 owned-first restore", mx)
    must("ac3608_3_owned_path_preserved", "AC3 test row", test)

    # AC4
    must("aura_evaluator_mutation_boundary_depth() > 0", "AC4 boundary gate", mx)
    must("aura_evaluator_mutation_boundary_depth() == 0", "AC4 local-only-no-boundary", mx)
    must("ac3608_4_boundary_ssot", "AC4 test row", test)

    # AC5
    must("is_sandbox_active", "AC5 production gate", mx)
    must("ac3608_5_soft_half_write_preserved", "AC5 test row", test)

    # AC6
    must("check_expand_checkpoint_local_rollback_3608", "AC6 build wiring", build)
    must("ac3608_6_source_and_linter", "AC6 test row", test)
    if (ROOT / "tests" / "compiler" / "test_issue_3608.cpp").is_file():
        fails.append("AC6: test_issue_3608.cpp present (forbidden per #81934/#81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*3608*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3608 no-Evaluator expand rollback — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
