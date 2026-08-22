#!/usr/bin/env python3
"""Issue #3263: quote_lambda marker sample-once + drop dead stripped bump.

quote_lambda bridge copy must read `marker(v.id)` once (no TOCTOU between
propagated value and counted value). `current_flat == nullptr` is an
invariant, not "marker stripped" — do not bump
g_2177_aot_macro_marker_stripped_total on that path. Keep #2177
record(1) for MacroIntroduced. Soft/Off skip (zero extra). Hook
signature unchanged.

Contract:
  AC1  marker(v.id) sampled once into a local
  AC2  contract_assert current_flat; no record(0) stripped bump
  AC3  MacroIntroduced still record(1); hook signature unchanged
  AC4  quiet non-macro / no-flat: zero extra stripped
  AC5  extend test_jit_macro_deopt_hygiene; linter after #3262; no invent

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

    low = _read("src/compiler/lowering_impl.cpp")
    br = _read("src/compiler/aura_jit_bridge.cpp")
    hdr_stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    test = _read("tests/compiler/test_jit_macro_deopt_hygiene.cpp")
    build = _read("build.py")
    l3262 = _read("scripts/coverage/checks/check_gc_safepoint_restamp_lock_3262.py")

    pos = low.find("Issue #3263: propagate SyntaxMarker")
    end = low.find("if (state.current_flat && state.current_pool)", pos)
    win = low[pos:end] if pos >= 0 and end > pos else ""
    must("Issue #3263", "AC1 cite", win)
    must("const auto m = state.current_flat->marker(v.id)", "AC1 once", win)
    if win.count("marker(v.id)") != 1:
        fails.append("AC1: marker(v.id) must be sampled once")
    must("ac3263_1_marker_sampled_once", "AC1 test", test)

    must("contract_assert(state.current_flat != nullptr)", "AC2 invariant", win)
    if "aura_2177_record_aot_marker_propagated(0)" in win:
        fails.append("AC2: dead else still bumps stripped via record(0)")
    if "g_2177_aot_macro_marker_stripped_total" in win:
        fails.append("AC2: quote_lambda must not bump stripped")
    must("zero extra", "AC2 soft skip", win)
    must("ac3263_2_no_stripped_on_null_flat", "AC2 test", test)

    must("aura_2177_record_aot_marker_propagated(1)", "AC3 record(1)", win)
    must("aura::ast::SyntaxMarker::MacroIntroduced", "AC3 macro path", win)
    must("void aura_2177_record_aot_marker_propagated(int propagated)", "AC3 hook", br)
    must("aura_2177_record_aot_marker_propagated(int propagated)", "AC3 stub", hdr_stub)
    must("ac3263_3_keep_2177_record_one", "AC3 test", test)

    must("if (state.current_flat)", "AC4 observe backup", win)
    must("ac3263_4_quiet_zero_extra", "AC4 test", test)

    must("ac3263_5_source_and_linter", "AC5 test", test)
    must("check_quote_lambda_marker_once_3263", "AC5 build.py", build)
    prev = build.find("check_gc_safepoint_restamp_lock_3262")
    ours = build.find("check_quote_lambda_marker_once_3263")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3262")
    must("3263", "AC5 extend 3262 linter", l3262)
    if (ROOT / "tests" / "issues" / "test_issue_3263.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3263.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3263.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3263.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3263-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    q = _read("src/compiler/evaluator_primitives_query_tail.cpp")
    if "schema-3263" in q or "schema-3263" in test:
        fails.append("AC5: new schema-3263 query key (SlimSurface)")

    if fails:
        print("FAIL #3263 quote_lambda_marker_once:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3263 quote_lambda_marker_once: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
