#!/usr/bin/env python3
"""Issue #2808: stamp_rest_param_hygiene sets SyntaxMarker::MacroIntroduced.
Issue #3153: residual — eval_flat dotted-rest + reexpand_call pair-spine
             call sites now invoke the helper too (was: skipped). Helper
             exposed cross-TU (static dropped, export added in .ixx).
             Same helper, same atomic — parity with macro_expand_all.

Contract (one row per AC):
  AC1 stamp_rest_param_hygiene cites #2808; set_marker MacroIntroduced + metrics
  AC2 marker set/skipped totals export + v_read + test call entry
  AC3 tests/compiler/test_stamp_rest_param_hygiene_marker.cpp
  AC4 this linter wired; no docs/design/2808-*; no test_issue_2808.cpp
  AC5 (#3153) eval_flat dotted-rest + reexpand_call pair-spine now call
     stamp_rest_param_hygiene (single shared helper, parity with
     macro_expand_all / expand_inner_macros). Helper anchor updated
     from `static inline void` to `inline void` (static dropped so the
     helper is callable from evaluator_eval_flat.cpp via the
     aura.compiler.macro_expansion module export).

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

    me = _read("src/compiler/macro_expansion.cpp")
    ixx = _read("src/compiler/macro_expansion.ixx")
    bridge = _read("src/compiler/aura_jit_bridge.h")
    eef = _read("src/compiler/evaluator_eval_flat.cpp")
    test = _read("tests/compiler/test_stamp_rest_param_hygiene_marker.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # Issue #3153: anchor updated from `static inline void` to `inline void`
    # — static was dropped so the helper is callable from a different TU
    # via the aura.compiler.macro_expansion module export. Definition still
    # single-source in macro_expansion.cpp as inline (ODR-safe).
    pos = me.find("inline void stamp_rest_param_hygiene(aura::ast::FlatAST&")
    win = me[pos : pos + 2000] if pos >= 0 else ""

    # AC1
    must("Issue #2808", "AC1", win)
    must("set_marker", "AC1", win)
    must("MacroIntroduced", "AC1", win)
    must("g_stamp_rest_param_marker_set_total", "AC1", win)
    must("g_stamp_rest_param_marker_skipped_total", "AC1", win)

    # AC2
    must("g_stamp_rest_param_marker_set_total", "AC2", me)
    must("g_stamp_rest_param_marker_set_total", "AC2", ixx)
    must("aura_stamp_rest_param_marker_set_total_v_read", "AC2", me)
    must("aura_stamp_rest_param_marker_set_total_v_read", "AC2", bridge)
    must("aura_test_call_stamp_rest_param_hygiene", "AC2", me)
    must("aura_test_call_stamp_rest_param_hygiene", "AC2", bridge)

    # AC3
    must("ac2808", "AC3", test)
    must("2808", "AC3", test)
    must("is_macro_introduced", "AC3", test)
    must("stamp_rest_param", "AC3", test)
    must("MacroIntroduced", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_stamp_rest_param_hygiene_marker.cpp").is_file():
        fails.append("AC3: missing test_stamp_rest_param_hygiene_marker.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2808.cpp").is_file():
        fails.append("AC3: test_issue_2808.cpp present (forbidden per #81967)")
    must("test_stamp_rest_param_hygiene_marker", "AC3", cmake)

    # AC4
    must("check_stamp_rest_param_hygiene_marker_2808", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2808-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    # ── AC5 (#3153): eval_flat dotted-rest + reexpand_call pair-spine
    # now invoke stamp_rest_param_hygiene (was: skipped). Single shared
    # helper, parity with macro_expand_all / expand_inner_macros. Helper
    # visibility: dropped static (kept inline), added export declaration
    # in macro_expansion.ixx so the helper is callable cross-TU via the
    # aura.compiler.macro_expansion module.
    must("Issue #3153", "AC5 source-cite marker (eval_flat dotted-rest block)", eef)
    must("stamp_rest_param_hygiene(*f, *md.flat, md.body_id, list_call)", "AC5 eval_flat dotted-rest stamp call", eef)
    must("Issue #3153", "AC5 source-cite marker (reexpand_call pair-spine block)", eef)
    must(
        "stamp_rest_param_hygiene(flat, md.flat ? *md.flat : flat, md.body_id, list_end)",
        "AC5 reexpand_call pair-spine stamp call",
        eef,
    )
    must("list_end != aura::ast::NULL_NODE", "AC5 reexpand_call guard on non-null list_end", eef)
    # Helper dropped static + export added.
    must("inline void stamp_rest_param_hygiene(", "AC5 helper dropped static (kept inline) — cross-TU ODR-safe", me)
    must("export void stamp_rest_param_hygiene(", "AC5 helper exported in macro_expansion.ixx", ixx)
    if me.find("static inline void stamp_rest_param_hygiene") >= 0:
        fails.append("AC5: helper still declared static inline (must be inline, cross-TU)")
    # New test file present + wired in CMakeLists (src/-aligned per #81967).
    if not (ROOT / "tests" / "compiler" / "test_rest_param_hygiene_eval_flat.cpp").is_file():
        fails.append("AC5: missing test_rest_param_hygiene_eval_flat.cpp")
    must("test_rest_param_hygiene_eval_flat", "AC5 test wired in CMakeLists", cmake)
    if (ROOT / "tests" / "compiler" / "test_issue_3153.cpp").is_file():
        fails.append("AC5: test_issue_3153.cpp present (forbidden per #81967)")
    if docs.is_dir():
        for f in sorted(docs.glob("3153-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")
    # Soft / Off zero-cost contract preserved — no new metrics middle layer
    # for the two new call sites (they reuse the existing atomic).
    if me.find("g_3153_") >= 0:
        fails.append("AC5: new g_3153_* atomic in macro_expansion.cpp (Soft: zero-cost preserved)")
    if ixx.find("g_3153_") >= 0:
        fails.append("AC5: new g_3153_* atomic in macro_expansion.ixx (Soft: zero-cost preserved)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2808 stamp_rest_param_hygiene marker — MacroIntroduced + set/skipped metrics")
    return 0


if __name__ == "__main__":
    sys.exit(main())
