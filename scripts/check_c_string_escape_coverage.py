#!/usr/bin/env python3
"""Linter for issue #1997 / B-002: shell-wrapper C-source escape must handle
C0 control chars + backslash + double-quote.

Audit reference: Aura JIT/FFI boundary code review, 2026-07-23 — finding ID B-002.

Acceptance criteria:
  AC1: aura_jit_bridge.cpp contains the "Escape for C string literal (issue
       #1997 / B-002)" anchor comment.
  AC2: The escape loop handles: backslash, double-quote, newline, tab, carriage
       return, NUL, BEL, BS, FF, VT (the 10 C0/quote chars required for
       portable C string-literal content).
  AC3: The escape loop uses a switch-statement form (cleanly extensible).
  AC4: tests/test_issue_1997.cpp exists and exercises the escape with at least
       one input containing each of: \\, ", \n, \t, \r, \0, \a, \b, \f, \v.
  AC5: The C-source printf emission after the escape loop uses std::format (or
       equivalent) with the escaped string.
  AC6: --self-test accepts good fixture, rejects bad (regression guard).

Usage:
  python3 scripts/check_c_string_escape_coverage.py             # gate
  python3 scripts/check_c_string_escape_coverage.py --self-test  # AC6
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BRIDGE = REPO / "src" / "compiler" / "aura_jit_bridge.cpp"
TEST_FILE = REPO / "tests" / "test_issue_1997.cpp"

# The 10 chars that MUST be escaped for portable C string-literal content.
REQUIRED_CASES = ["\\\\", '"', "\\n", "\\t", "\\r", "\\0", "\\a", "\\b", "\\f", "\\v"]


def _extract_body(text: str, start: int) -> str:
    """Extract the brace-balanced block starting at the first '{' at or after
    `start`. Returns the substring including the braces, or '' if unbalanced.
    """
    i = text.find("{", start)
    if i == -1:
        return ""
    depth = 0
    for j in range(i, len(text)):
        c = text[j]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[i : j + 1]
    return ""


def _find_anchor_and_loop(text: str) -> tuple[int, str] | None:
    """Locate the B-002 anchor and the for-loop body that follows it.

    Returns (anchor_line_1based, loop_body_text) or None. The loop_body is
    the brace-balanced block of the for(...){...} that comes after the anchor.
    """
    m = re.search(r"//\s*Escape for C string literal \(issue #1997 / B-002\)\.", text)
    if not m:
        return None
    anchor_line = text[: m.start()].count("\n") + 1
    # Search for the for-loop declaration in the text AFTER the anchor
    for_match = re.search(r"for\s*\([^)]*\)\s*\{", text[m.end() :])
    if not for_match:
        return None
    # Absolute position of the for-loop's opening brace
    abs_brace = m.end() + for_match.end() - 1
    loop_body = _extract_body(text, abs_brace)
    if not loop_body:
        return None
    return anchor_line, loop_body


def _count_required_cases(loop_body: str) -> tuple[int, list[str]]:
    """Count how many of the 10 REQUIRED_CASES are handled in the loop body.

    Returns (count_handled, list_of_missing).
    """
    missing: list[str] = []
    handled = 0
    for case in REQUIRED_CASES:
        if re.search(r"case\s+'" + re.escape(case) + r"'\s*:", loop_body):
            handled += 1
        else:
            missing.append(case)
    return handled, missing


def _uses_switch(loop_body: str) -> bool:
    return bool(re.search(r"\bswitch\s*\(", loop_body))


def _uses_std_format_for_printf(text: str, anchor_end: int) -> bool:
    """Verify the printf emission after the anchor uses std::format with the
    escaped string. This binds the runtime gate to the patched loop.
    """
    window = text[anchor_end : anchor_end + 1200]
    has_format = ("std::format" in window) or ("fmt::format" in window)
    has_printf = ("printf(" in window) and ("%s" in window)
    return has_format and has_printf


def _test_covers_all_cases(test_text: str) -> tuple[int, list[str]]:
    """Verify the test file exercises inputs containing all 10 required chars."""
    missing: list[str] = []
    covered = 0
    for case in REQUIRED_CASES:
        if case in test_text:
            covered += 1
        else:
            missing.append(case)
    return covered, missing


def check() -> tuple[bool, list[str]]:
    """Run AC1-AC5. Returns (ok, errors)."""
    errors: list[str] = []

    if not BRIDGE.exists():
        return False, [f"AC1 FAIL: {BRIDGE} not found"]
    bridge_text = BRIDGE.read_text()

    found = _find_anchor_and_loop(bridge_text)
    if found is None:
        errors.append(
            "AC1 FAIL: shell-wrapper C-source escape loop with "
            "'Escape for C string literal (issue #1997 / B-002)' anchor + "
            f"matching for-loop not found in {BRIDGE}"
        )
    else:
        anchor_line, loop_body = found

        handled, missing = _count_required_cases(loop_body)
        if handled != len(REQUIRED_CASES):
            errors.append(
                f"AC2 FAIL: escape loop handles {handled}/{len(REQUIRED_CASES)} "
                f"required cases. Missing: {missing}. Anchor at "
                f"{BRIDGE.name}:{anchor_line}."
            )
        if not _uses_switch(loop_body):
            errors.append(
                "AC3 FAIL: escape loop does not use a switch-statement form. "
                "The B-002 fix is required to be a switch so the regression "
                "guard (covering all 10 cases) is structurally enforced."
            )
        anchor_match = re.search(
            r"//\s*Escape for C string literal \(issue #1997 / B-002\)\.",
            bridge_text,
        )
        if not _uses_std_format_for_printf(bridge_text, anchor_match.end()):
            errors.append(
                "AC5 FAIL: C-source printf emission after the escape loop does "
                "not appear to use std::format (or equivalent) with %s. "
                "Runtime gate may be unbound from the patched loop."
            )

    if not TEST_FILE.exists():
        errors.append(f"AC4 FAIL: test file {TEST_FILE} not found")
    else:
        test_text = TEST_FILE.read_text()
        covered, missing = _test_covers_all_cases(test_text)
        if covered != len(REQUIRED_CASES):
            errors.append(
                f"AC4 FAIL: test file exercises {covered}/{len(REQUIRED_CASES)} "
                f"required chars. Missing: {missing}. Test must include each "
                f"of: {REQUIRED_CASES}"
            )

    ok = not errors
    return ok, errors


def self_test() -> tuple[bool, list[str]]:
    """AC6: synthetic good/bad fixture, ensure check() rejects bad and accepts good."""
    errors: list[str] = []

    # ---- Good fixture: switch with all 10 cases + std::format printf ----
    good_source = (
        "// Escape for C string literal (issue #1997 / B-002).\n"
        "std::string escaped;\n"
        "for (char c : result) {\n"
        "    switch (c) {\n"
        "        case '\\\\':  escaped += \"\\\\\\\\\"; break;\n"
        '        case \'"\':   escaped += "\\\\\\""; break;\n'
        "        case '\\n':  escaped += \"\\\\n\";  break;\n"
        "        case '\\t':  escaped += \"\\\\t\";  break;\n"
        "        case '\\r':  escaped += \"\\\\r\";  break;\n"
        "        case '\\0':  escaped += \"\\\\0\";  break;\n"
        "        case '\\a':  escaped += \"\\\\a\";  break;\n"
        "        case '\\b':  escaped += \"\\\\b\";  break;\n"
        "        case '\\f':  escaped += \"\\\\f\";  break;\n"
        "        case '\\v':  escaped += \"\\\\v\";  break;\n"
        "        default:    escaped.push_back(c); break;\n"
        "    }\n"
        "}\n"
        'of << std::format(R"(    printf("%s\\n", "{}");)", escaped);\n'
    )
    found = _find_anchor_and_loop(good_source)
    if found is None:
        errors.append("self-test: good fixture -- anchor + for-loop not found")
        return False, errors
    _, good_loop = found
    handled, missing = _count_required_cases(good_loop)
    if handled != len(REQUIRED_CASES):
        errors.append(f"self-test: good fixture rejected -- handled={handled}, missing={missing}")
    if not _uses_switch(good_loop):
        errors.append("self-test: good fixture not detected as switch-form")

    # ---- Bad fixture: missing \\t and \\0 (regression of B-002) ----
    bad_source = (
        "// Escape for C string literal (issue #1997 / B-002).\n"
        "std::string escaped;\n"
        "for (char c : result) {\n"
        "    switch (c) {\n"
        "        case '\\\\':  escaped += \"\\\\\\\\\"; break;\n"
        '        case \'"\':   escaped += "\\\\\\""; break;\n'
        "        case '\\n':  escaped += \"\\\\n\";  break;\n"
        "        case '\\r':  escaped += \"\\\\r\";  break;\n"
        "        case '\\a':  escaped += \"\\\\a\";  break;\n"
        "        default:    escaped.push_back(c); break;\n"
        "    }\n"
        "}\n"
    )
    found_bad = _find_anchor_and_loop(bad_source)
    if found_bad is None:
        errors.append("self-test: bad fixture -- anchor + for-loop not found (unexpected)")
        return False, errors
    _, bad_loop = found_bad
    handled_bad, missing_bad = _count_required_cases(bad_loop)
    if handled_bad == len(REQUIRED_CASES):
        errors.append(
            f"self-test: bad fixture incorrectly accepted as good (handled={handled_bad}, missing={missing_bad})"
        )
    if "\\t" not in missing_bad or "\\0" not in missing_bad:
        errors.append(f"self-test: bad fixture missing-flag wrong (missing={missing_bad}, expected \\\\t and \\\\0)")

    ok = not errors
    return ok, errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run synthetic good/bad fixture check (AC6) and exit",
    )
    args = parser.parse_args()

    if args.self_test:
        ok, errors = self_test()
        if ok:
            print("self-test: OK (good fixture accepted, bad fixture rejected)")
            return 0
        for e in errors:
            print(e)
        return 1

    ok, errors = check()
    if ok:
        print("OK: all #1997 ACs satisfied")
        return 0
    for e in errors:
        print(e)
    return 1


if __name__ == "__main__":
    sys.exit(main())
