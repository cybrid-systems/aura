#!/usr/bin/env python3
r"""Issue #4129 source-cite gate: character literal reader (#\ forms).

`#` (unless #t, #f, or #() lexed as TokenKind::Error in
src/parser/lexer_impl.cpp. parse_list's error recovery then treated that
error token as the closing ')': parse_expr returned NULL_NODE without
consuming it and the loop's trailing consume ate `#`, silently dropping
every argument at/after a character literal. (equal? #\0 #\9) matched
zero-arg (equal?) → #t; (char->integer #\0) matched the zero-arg call → 0
instead of 48; #\a evaluated the identifier after the skipped #\ →
"unbound variable: a". stderr stayed empty.

Fix shape:
  - lexer: TokenKind::Character + read_character() — named forms (space/
    newline/tab + R7RS set, case-insensitive), single-char forms (#\a
    #\0 #\- #\(), optional hex #\xHH. Token text carries the DECIMAL
    CODE POINT (chars ride the existing "chars are integers" model —
    char?/char->integer/char=? are int-based; string-ref yields int code
    points; no second value model). Malformed literals (bare #\ at EOF,
    unknown long name) return a TokenKind::Error whose text starts with
    "#\" so the parser records a real parse error.
  - parser: parse_expr consumes Character → integer literal node
    (add_literal, no new marker); parse_list no longer mistakes a
    literal for ')'. ParserState.deferred_error + parse() flush force
    success=false and a NULL root for malformed literals — never a
    silently-truncated call.
  - evaluator: unchanged — the literal IS an integer; the char
    primitives already operate on that representation.

ACs:
  AC1  lexer.ixx declares TokenKind::Character and the read_character()
       method, citing #4129.
  AC2  lexer_impl.cpp routes `next == '\\'` to read_character() inside
       the `#` branch (before the Error fallback); read_character
       returns a "#\"-shaped Error for bare #\ at EOF and unknown names.
  AC3  read_character implements the named-form table (space 32,
       newline/linefeed 10, tab 9, nul/null 0, alarm 7, backspace 8,
       vtab 11, page 12, return 13, escape/altmode 27, delete/rubout
       127) and the optional hex #\xHH form.
  AC4  parser_impl.cpp parse_expr has a TokenKind::Character case that
       lowers the token to its code point via add_literal (chars-are-
       integers rationale cites #4129); token_desc describes Character;
       record_error's recovery restart-set accepts Character.
  AC5  ParserState carries deferred_error (cites #4129) and parse()
       flushes it via flush_deferred_char_error at all three return
       paths, forcing r.root = NULL_NODE and r.success = false so a
       malformed literal fails the form instead of silently evaluating
       truncated arguments.
  AC6  tests/compiler/test_ir.cpp hosts #4129 AC1..AC10 in
       test_character_literal_4129 (no tests/**/test_issue_4129.cpp, no
       docs/design/4129-*); build.py wires this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LEXER_IXX = ROOT / "src" / "parser" / "lexer.ixx"
LEXER_CPP = ROOT / "src" / "parser" / "lexer_impl.cpp"
PARSER_IXX = ROOT / "src" / "parser" / "parser.ixx"
PARSER_CPP = ROOT / "src" / "parser" / "parser_impl.cpp"
TEST = ROOT / "tests" / "compiler" / "test_ir.cpp"


def _fn_body(text: str, header: str) -> str:
    """Body of a Lexer:: member from its header line to the next member."""
    idx = text.find(header)
    if idx < 0:
        return ""
    nxt = len(text)
    for tag in ("\nToken Lexer::", "\nvoid Lexer::"):
        pos = text.find(tag, idx + len(header))
        if pos >= 0:
            nxt = min(nxt, pos)
    return text[idx:nxt]


def main() -> int:
    lexer_ixx = LEXER_IXX.read_text() if LEXER_IXX.exists() else ""
    lexer_cpp = LEXER_CPP.read_text() if LEXER_CPP.exists() else ""
    parser_ixx = PARSER_IXX.read_text() if PARSER_IXX.exists() else ""
    parser_cpp = PARSER_CPP.read_text() if PARSER_CPP.exists() else ""
    test = TEST.read_text() if TEST.exists() else ""
    build = (ROOT / "build.py").read_text()
    allow = (ROOT / "scripts" / "coverage" / "root_check_allowlist.txt").read_text()
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    # AC1 — TokenKind::Character + read_character() declaration.
    enum_idx = lexer_ixx.find("export enum class TokenKind")
    enum_end = lexer_ixx.find("};", enum_idx) if enum_idx >= 0 else -1
    enum_body = lexer_ixx[enum_idx:enum_end] if enum_end > 0 else ""
    good = (
        "Character, // Issue #4129: #\\ character literal; token text = decimal code point" in enum_body
        and "Token read_character();" in lexer_ixx
    )
    report("AC1", good, "TokenKind::Character + read_character() declared in lexer.ixx")

    # AC2 — #\ routed to read_character before the Error fallback; malformed
    # literals return "#\"-shaped Error tokens.
    rc_body = _fn_body(lexer_cpp, "Token Lexer::read_character()")
    hash_idx = lexer_cpp.find("if (c == '#' && pos_ + 1 < source_.size())")
    route_idx = lexer_cpp.find("if (next == '\\\\')\n                    return read_character();")
    err_idx = lexer_cpp.find("return make_tok(TokenKind::Error, source_.substr(pos_++, 1));")
    good = (
        "#4129" in lexer_cpp
        and 0 <= hash_idx < route_idx < err_idx
        and 'return make_tok(TokenKind::Error, "#\\\\"); // bare #\\ at EOF — malformed' in rc_body
        and "return make_tok(TokenKind::Error, source_.substr(start, pos_ - start)); // unknown name" in rc_body
    )
    report(
        "AC2",
        good,
        '#\\ routes to read_character before the Error fallback; malformed literals emit "#\\"-shaped Errors',
    )

    # AC3 — named-form table + optional hex form.
    good = (
        'lower == "nul" || lower == "null"' in rc_body
        and 'lower == "alarm"' in rc_body
        and 'lower == "backspace"' in rc_body
        and 'lower == "tab"' in rc_body
        and 'lower == "linefeed" || lower == "newline"' in rc_body
        and 'lower == "vtab"' in rc_body
        and 'lower == "page"' in rc_body
        and 'lower == "return"' in rc_body
        and 'lower == "escape" || lower == "altmode"' in rc_body
        and 'lower == "space"' in rc_body
        and 'lower == "delete" || lower == "rubout"' in rc_body
        and "(c == 'x' || c == 'X')" in rc_body
        and "hex_val" in rc_body
    )
    report("AC3", good, "named-form table (space/newline/tab + R7RS set) + hex #\\xHH form present")

    # AC4 — parse_expr lowers Character to its code point (one value model).
    char_case_idx = parser_cpp.find("// Issue #4129: character literals evaluate")
    char_case_end = parser_cpp.find("case TokenKind::Float:", char_case_idx) if char_case_idx >= 0 else -1
    char_case = parser_cpp[char_case_idx:char_case_end] if char_case_end > 0 else ""
    good = (
        "#4129" in char_case
        and "s.flat.add_literal(std::stoll(std::string(tok.text)))" in char_case
        and "No second character value model" in char_case
        and "case TokenKind::Character: // Issue #4129" in parser_cpp
        and "tok.kind == TokenKind::Character ||" in parser_cpp
    )
    report(
        "AC4",
        good,
        "parse_expr Character case = code-point integer literal (chars-are-integers, one model); desc + recovery updated",
    )

    # AC5 — deferred_error + flush at all three return paths.
    good = (
        "// Issue #4129: first malformed character literal seen" in parser_ixx
        and "std::string deferred_error;" in parser_ixx
        and "auto flush_deferred_char_error" in parser_cpp
        and parser_cpp.count("flush_deferred_char_error();") >= 3
        and 'if (tok.text.starts_with("#\\\\") && s.deferred_error.empty())' in parser_cpp
        and "r.root = NULL_NODE;\n            r.success = false;" in parser_cpp
    )
    report(
        "AC5",
        good,
        "ParserState.deferred_error flushed at all return paths; malformed literal fails the form (never silent)",
    )

    # AC6 — tests host the ACs; no issue-file; no design doc; wiring present.
    no_test_file = not (ROOT / "tests" / "compiler" / "test_issue_4129.cpp").exists()
    no_doc = not any((ROOT / "docs" / "design").glob("4129-*")) if (ROOT / "docs" / "design").exists() else True
    acs = all(f"// AC{i} " in test for i in range(1, 11)) and all(
        pin in test
        for pin in (
            '"--- #4129 AC{}: {}"',
            '"  FAIL: #4129 AC{}: {}"',
            "#0 lexes as Character token with code point 48",
            "(equal? #0 #9) is #f and (equal? #0 #0) is #t",
            "named forms end-to-end: 32/10/9",
            "bare #\\\\ / unknown #foobar are parse errors; eval refuses",
        )
    )
    good = (
        "bool test_character_literal_4129()" in test
        and acs
        and "=== Results: {} passed, {} failed" in test
        and no_test_file
        and no_doc
        and "check_character_literal_4129.py" in build
        and "check_character_literal_4129.py" in allow
    )
    report(
        "AC6",
        good,
        "test_ir.cpp hosts #4129 AC1..AC10; no issue-file/doc; build.py + allowlist wired",
    )

    print(f"check_character_literal_4129: {'OK' if ok else 'FAILED'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
