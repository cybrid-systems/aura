#!/usr/bin/env python3
# scripts/check_export_name_unparse_4132.py -- Issue #4132 gate.
#
# AC1: the unparse Export case renders the stored NAME CHILDREN — the case
#      slices iterate `v.children` (the add_export PersistentChildVector
#      encoding), resolve Variable sym_ids via the string pool, and keep the
#      generic emit as a fallback; the case cites #4132.
# AC2: one storage encoding, one emitter — the parser still builds exports
#      through add_export(syms) (children_, not params), the Export case
#      contains NO `v.params` loop (the forbidden empty `(export)` emitter),
#      and exactly one Export case exists in the shared unparse emitter.
# AC3: test family — ac4132_1..ac4132_4 are declared AND dispatched in
#      run_test_current_source_roundtrip, the kRoundtripNoMutate table has
#      the `export names (#4132)` row, and the has_empty_export helper is
#      defined and used (direct never-empty-(export) assertions).
# AC4: wiring — build.py registers this linter, the filename is on the
#      frozen root_check_allowlist, eval semantics stay untouched (no 4132
#      markers inside the eval side), and no docs/design/4132-* or
#      tests/**/test_issue_4132* exists (per #1655 / #81934).

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

UNPARSE = "src/core/ast_unparse.ixx"
PARSER = "src/parser/parser_impl.cpp"
AST = "src/core/ast.ixx"
TEST = "tests/compiler/test_current_source_roundtrip.cpp"
BUILD = "build.py"
ALLOWLIST = "scripts/coverage/root_check_allowlist.txt"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _export_case_slice(src: str) -> str:
    """Slice the NodeTag::Export case body (next-case bound)."""
    key = "case NodeTag::Export:"
    pos = src.find(key)
    if pos == -1:
        return ""
    nxt = src.find("case NodeTag::", pos + len(key))
    end = nxt if nxt != -1 else pos + 4000
    return src[pos:end]


def main() -> int:
    fails: list[str] = []

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    # ── AC1: Export case renders the stored name children ──
    unp = _read(UNPARSE)
    must("4132" in unp, "AC1: ast_unparse.ixx cites #4132")
    ec = _export_case_slice(unp)
    must(ec != "", "AC1: NodeTag::Export case found")
    must('append("(export")' in ec, "AC1: Export case anchored on the (export emitter")
    children_loop = ec.find("for (auto cid : v.children)")
    resolve_arm = ec.find("child.tag == NodeTag::Variable")
    pool_resolve = ec.find("pool.resolve(child.sym_id)")
    generic_fallback = ec.find("emit(cid, depth + 1, child_indent)")
    must(children_loop != -1, "AC1: Export case iterates the stored name children")
    must(resolve_arm != -1, "AC1: Variable resolve arm present")
    must(pool_resolve != -1 and pool_resolve > resolve_arm, "AC1: arm resolves child sym_ids via the pool")
    must(
        generic_fallback != -1 and generic_fallback > resolve_arm,
        "AC1: resolve arm ordered before the generic emit fallback",
    )
    must("params side-table" in ec, "AC1: children-not-params rationale documented in the case")

    # ── AC2: one storage encoding, one emitter, no empty-(export) regression ──
    parser = _read(PARSER)
    must("add_export(syms)" in parser, "AC2: parser still builds exports via add_export(syms)")
    ast = _read(AST)
    must("NodeId add_export(std::span<const NodeId> syms)" in ast, "AC2: add_export SSOT intact")
    must("children_[id] = PersistentChildVector<NodeId>(" in ast, "AC2: add_export stores names in children_")
    must("for (auto pid : v.params)" not in ec, "AC2: forbidden params loop gone from the Export case")
    must(unp.count("case NodeTag::Export:") == 1, "AC2: exactly one Export case in the shared emitter")

    # ── AC3: test family declared AND dispatched ──
    t = _read(TEST)
    must("export names (#4132)" in t, "AC3: roundtrip table row present")
    must("static bool has_empty_export(" in t, "AC3: has_empty_export helper defined")
    for fn in (
        "ac4132_1_unparse_preserves_export_names",
        "ac4132_2_export_roundtrip_reparse",
        "ac4132_3_storage_and_eval_unchanged",
        "ac4132_4_source_cite",
    ):
        must(f"static void {fn}(" in t, f"AC3: {fn} declared")
        must(f"{fn}();" in t, f"AC3: {fn} dispatched in the runner")
    must(t.count("has_empty_export(") >= 6, "AC3: has_empty_export used across ACs")

    # ── AC4: wiring + non-goals ──
    build = _read(BUILD)
    must("check_export_name_unparse_4132" in build, "AC4: build.py registration")
    allow = _read(ALLOWLIST)
    must("check_export_name_unparse_4132.py" in allow, "AC4: root allowlist entry")
    must(not any((ROOT / "docs" / "design").glob("4132-*")), "AC4: no docs/design/4132-* per #1655")
    must(not any((ROOT / "tests").rglob("test_issue_4132*")), "AC4: no tests/**/test_issue_4132* per #81934")
    eval_side = _read("src/compiler/evaluator_eval_flat.cpp")
    must("4132" not in eval_side, "AC4: eval side untouched (no 4132 markers)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: check_export_name_unparse_4132 (4 ACs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
