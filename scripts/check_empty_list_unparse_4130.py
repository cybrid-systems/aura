#!/usr/bin/env python3
# scripts/check_empty_list_unparse_4130.py -- Issue #4130 gate.
#
# AC1: the :pretty / compact unparse Quote case carries the nil-sentinel arm
#      BEFORE the generic child emit — the arm matches the same encoding the
#      renderer walk already honors (LiteralInt + int_value == 0 + not
#      BoolLiteral) and emits `()`, so quoted empty lists render as
#      `(quote ())` and NEVER as `(quote 0)`; the case cites #4130.
# AC2: one nil encoding, both renderers honor it — the parser SSOT
#      (`() — empty list` → add_literal(0)) is untouched, the
#      unparse_proper_list list-tail nil break stays, and NO file under src/
#      contains the forbidden `(quote 0)` emitter string.
# AC3: test family — ac4130_1..ac4130_4 are declared AND dispatched in
#      run_test_current_source_roundtrip, the kRoundtripNoMutate table has
#      the `quote empty list (#4130)` row, and the has_quote_zero helper is
#      defined and used (direct never-(quote 0) assertions).
# AC4: wiring — build.py registers this linter, the filename is on the
#      frozen root_check_allowlist, eval semantics stay untouched (no 4130
#      markers inside evaluator/eval side), and no docs/design/4130-* or
#      tests/**/test_issue_4130* exists (per #1655 / #81934).

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

UNPARSE = "src/core/ast_unparse.ixx"
PARSER = "src/parser/parser_impl.cpp"
TEST = "tests/compiler/test_current_source_roundtrip.cpp"
BUILD = "build.py"
ALLOWLIST = "scripts/coverage/root_check_allowlist.txt"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _quote_case_slice(src: str) -> str:
    """Slice the NodeTag::Quote case body (next-case bound)."""
    key = "case NodeTag::Quote:"
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

    # ── AC1: Quote-case nil-sentinel arm before the generic child emit ──
    unp = _read(UNPARSE)
    must("4130" in unp, "AC1: ast_unparse.ixx cites #4130")
    qc = _quote_case_slice(unp)
    must(qc != "", "AC1: NodeTag::Quote case found")
    arm_guard = qc.find("qv.tag == NodeTag::LiteralInt && qv.int_value == 0")
    arm_emit = qc.find('append("()");', arm_guard if arm_guard != -1 else 0)
    child_emit = qc.find("emit(v.child(0)")
    must(arm_guard != -1, "AC1: nil-sentinel guard (LiteralInt, int_value == 0) present")
    must("SyntaxMarker::BoolLiteral" in qc, "AC1: BoolLiteral exclusion in the arm")
    must(arm_emit != -1 and arm_emit > arm_guard, "AC1: arm emits () for the sentinel")
    must(
        child_emit != -1 and (arm_emit == -1 or child_emit > arm_emit), "AC1: arm ordered before the generic child emit"
    )

    # ── AC2: one nil encoding; never emit (quote 0); both renderers honor it ──
    parser = _read(PARSER)
    must("() — empty list" in parser, "AC2: parser nil-sentinel SSOT comment (add_literal(0)) intact")
    must("add_literal(0)" in parser, "AC2: parser still encodes () as LiteralInt 0")
    list_walk = unp[unp.find("unparse_proper_list") : unp.find("void emit(")]
    must(
        "nil sentinel" in list_walk and "int_value == 0" in list_walk,
        "AC2: list-tail renderer keeps the nil-sentinel break",
    )
    for rel in sorted((ROOT / "src").rglob("*")):
        if rel.is_file() and rel.suffix in {".cpp", ".ixx", ".hh", ".h", ".hpp"}:
            try:
                body = rel.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            must("(quote 0)" not in body, f"AC2: forbidden (quote 0) string in {rel.relative_to(ROOT)}")

    # ── AC3: test family declared AND dispatched ──
    t = _read(TEST)
    must("quote empty list (#4130)" in t, "AC3: roundtrip table row present")
    must("static bool has_quote_zero(" in t, "AC3: has_quote_zero helper defined")
    for fn in (
        "ac4130_1_pretty_unparse_empty_list",
        "ac4130_2_call_site_roundtrip",
        "ac4130_3_eval_semantics_unchanged",
        "ac4130_4_source_cite",
    ):
        must(f"static void {fn}(" in t, f"AC3: {fn} declared")
        must(f"{fn}();" in t, f"AC3: {fn} dispatched in the runner")
    must(t.count("has_quote_zero(") >= 6, "AC3: has_quote_zero used across ACs")

    # ── AC4: wiring + non-goals ──
    build = _read(BUILD)
    must("check_empty_list_unparse_4130" in build, "AC4: build.py registration")
    allow = _read(ALLOWLIST)
    must("check_empty_list_unparse_4130.py" in allow, "AC4: root allowlist entry")
    must(not any((ROOT / "docs" / "design").glob("4130-*")), "AC4: no docs/design/4130-* per #1655")
    must(not any((ROOT / "tests").rglob("test_issue_4130*")), "AC4: no tests/**/test_issue_4130* per #81934")
    eval_side = _read("src/compiler/evaluator_eval_flat.cpp")
    must("4130" not in eval_side, "AC4: eval side untouched (no 4130 markers)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: check_empty_list_unparse_4130 (4 ACs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
