#!/usr/bin/env python3
"""Issue #3786: *allow-macro-inline* #t gated by MacroSelfEvo under Restricted/Strict.

AC1  Primitive cites #3786 and deny_marker_clear_without_mse before toggle
AC2  Soft gated by effect_sandbox_mode() != 0; toggle after gate
AC3  hygiene:set-allow-macro-mutate! gate retained (#3652)
AC4  Tests ac3786_1..4 wired; build.py + grandfather; no test_issue_3786.cpp
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    path = ROOT / rel
    return path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    cpp = _read("src/compiler/evaluator_primitives_compile.cpp")
    test = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    build = _read("build.py")
    gf = _read("scripts/coverage/simple_check_grandfather.txt")

    must("Issue #3786", "AC1 cite", cpp)
    must("(*allow-macro-inline* #t) requires MacroSelfEvo capability", "AC1 deny string", cpp)
    pos = cpp.find('add("*allow-macro-inline*"')
    if pos < 0:
        fails.append("AC1: *allow-macro-inline* missing")
    else:
        win = cpp[pos : pos + 1400]
        must("effect_sandbox_mode() != 0", "AC2 Soft gate", win)
        must("deny_marker_clear_without_mse(ev, 0)", "AC2 helper in body", win)
        must("set_inline_respect_macro_hygiene(!enable)", "AC2 toggle after gate", win)

    must(
        "(hygiene:set-allow-macro-mutate! #t) requires MacroSelfEvo capability",
        "AC3 mutate flag retained",
        cpp,
    )

    for name in (
        "ac3786_1_allow_macro_inline_denied",
        "ac3786_2_mse_grant_toggle_ok",
        "ac3786_3_soft_ungated",
        "ac3786_4_mutate_flag_unchanged_and_cite",
        "ac3786_1_allow_macro_inline_denied();",
    ):
        must(name, f"AC4 {name}", test)

    must("check_allow_macro_inline_mse_3786", "AC4 build.py", build)
    must("check_allow_macro_inline_mse_3786.py", "AC4 grandfather", gf)

    if _read("tests/compiler/test_issue_3786.cpp"):
        fails.append("AC4: test_issue_3786.cpp present")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for f in sorted(design.glob("3786-*")):
            fails.append(f"AC4: docs/design/{f.name} present")

    if fails:
        print(f"FAIL #3786 allow_macro_inline_mse ({len(fails)} rows):")
        for row in fails:
            print(f"  - {row}")
        return 1
    print("OK #3786 allow_macro_inline_mse")
    return 0


if __name__ == "__main__":
    sys.exit(main())
