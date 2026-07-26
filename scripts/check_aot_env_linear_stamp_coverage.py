#!/usr/bin/env python3
# scripts/check_aot_env_linear_stamp_coverage.py
#
# Issue #2091 + #2168 linter (hard `./build.py gate` step):
# ensure every production AOT emit / reemit / registration site
# threads live env_frame_version + linear_state into
# mangle_aot_name / aot_link_name. Pre-#2091 most call sites
# passed literal (0, 0) so the `_eN_lN` suffix was omitted and
# captured-env / captured-linear drift became invisible to
# dlopen-based stale probes. This linter fails CI when any
# production emit path passes literal (0, 0) without an
# explicit `# 2091-allow-zero` annotation.
#
# Wired into gate via build.py `cmd_aot_env_linear_stamp` (#2168).
# Annotation contract is documented next to the force-flag
# comment in src/compiler/aot_mangle.h.
#
# Allowed patterns:
#   1. Both args are NOT literal 0 (i.e. live / computed value).
#   2. Literal 0,0 paired with `# 2091-allow-zero` or `# 2091-legacy`
#      on the same line or the previous line.
#   3. Test files (`tests/`) — full allowance (script scans only
#      src/compiler/; tests never enter the walk).
#   4. SKIP_BASENAMES: aot_mangle.h (defaults), aura_jit_bridge_stub.cpp,
#      and known test helpers under src/compiler/.
#
# Pattern detection: simple regex over mangle_aot_name(...) /
# aot_link_name(...) calls. Counts literal 0 args in slots 4 + 5
# (env_frame_version, linear_state). Fails when both are literal 0
# and no allow annotation is on the same line or one line above.
# 3-arg calls (defuse-only; env/linear defaulted) are also flagged
# in production TUs (same allow annotation opts out).

import argparse
import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = REPO_ROOT / "src" / "compiler"
TESTS_DIR = REPO_ROOT / "tests"

# mangle_aot_name(name, disambiguator, defuse_version[, env_frame_version, linear_state])
MANGLE_RE = re.compile(
    r"\b(mangle_aot_name|aot_link_name)\s*\(",
    re.MULTILINE,
)


# Find the closing paren for each match (handles nested parens).
def find_call_args(text: str, start: int) -> str | None:
    depth = 0
    i = start
    in_str = False
    in_char = False
    in_line_comment = False
    in_block_comment = False
    while i < len(text):
        c = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if in_line_comment:
            if c == "\n":
                in_line_comment = False
            i += 1
            continue
        if in_block_comment:
            if c == "*" and nxt == "/":
                in_block_comment = False
                i += 2
                continue
            i += 1
            continue
        if in_str:
            if c == "\\" and nxt:
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if in_char:
            if c == "\\" and nxt:
                i += 2
                continue
            if c == "'":
                in_char = False
            i += 1
            continue
        if c == "/" and nxt == "/":
            in_line_comment = True
            i += 2
            continue
        if c == "/" and nxt == "*":
            in_block_comment = True
            i += 2
            continue
        if c == '"':
            in_str = True
        elif c == "'":
            in_char = True
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[start + 1 : i]
        i += 1
    return None


def split_args(s: str) -> list[str]:
    out = []
    depth = 0
    cur = []
    in_str = False
    in_char = False
    in_line_comment = False
    in_block_comment = False
    i = 0
    while i < len(s):
        c = s[i]
        nxt = s[i + 1] if i + 1 < len(s) else ""
        if in_line_comment:
            if c == "\n":
                in_line_comment = False
            cur.append(c)
            i += 1
            continue
        if in_block_comment:
            if c == "*" and nxt == "/":
                in_block_comment = False
                cur.append(c)
                cur.append(nxt)
                i += 2
                continue
            cur.append(c)
            i += 1
            continue
        if in_str:
            cur.append(c)
            if c == "\\" and nxt:
                cur.append(nxt)
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if in_char:
            cur.append(c)
            if c == "\\" and nxt:
                cur.append(nxt)
                i += 2
                continue
            if c == "'":
                in_char = False
            i += 1
            continue
        if c == "/" and nxt == "/":
            in_line_comment = True
            cur.append(c)
            cur.append(nxt)
            i += 2
            continue
        if c == "/" and nxt == "*":
            in_block_comment = True
            cur.append(c)
            cur.append(nxt)
            i += 2
            continue
        if c == '"':
            in_str = True
        if c == "'":
            in_char = True
        if c in "([{":
            depth += 1
        if c in ")]}":
            depth -= 1
        if c == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
            i += 1
            continue
        cur.append(c)
        i += 1
    if cur:
        out.append("".join(cur).strip())
    return out


# Top-level file skip list (definitions + tests).
SKIP_BASENAMES = {
    "aot_mangle.h",
    "aura_jit_bridge_stub.cpp",
    "test_aot_mangle_top.cpp",
    "test_mutation_aot_unit_batch.cpp",
    "test_issue_2015.cpp",
    "test_issue_1640.cpp",
    "test_issue_2091.cpp",
}

# Allow-line marker (comment-based opt-out for deliberate (0,0) sites).
ALLOW_RE = re.compile(r"#\s*2091-(?:allow-zero|legacy)")


def is_in_comment(text: str, pos: int) -> bool:
    """True if position `pos` is inside a `//` line comment or
    `/* ... */` block comment at the time of the match. Walks
    forward from the start of the line to `pos`, tracking the
    same comment / string state machine as find_call_args.
    """
    # Find the start of the line containing `pos`.
    line_start = text.rfind("\n", 0, pos) + 1
    i = line_start
    in_block = False
    in_str = False
    in_char = False
    while i < pos:
        c = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if in_block:
            if c == "*" and nxt == "/":
                in_block = False
                i += 2
                continue
            i += 1
            continue
        if in_str:
            if c == "\\" and nxt:
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if in_char:
            if c == "\\" and nxt:
                i += 2
                continue
            if c == "'":
                in_char = False
            i += 1
            continue
        if c == "/" and nxt == "/":
            # Rest of the line is a comment — `pos` is inside.
            return True
        if c == "/" and nxt == "*":
            in_block = True
            i += 2
            continue
        if c == '"':
            in_str = True
        elif c == "'":
            in_char = True
        i += 1
    return in_block


def scan_file(path: Path) -> list[tuple[int, str, str, str]]:
    """Return list of (line_no, func, env_arg, linear_arg) for suspicious calls."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    findings = []
    for m in MANGLE_RE.finditer(text):
        # Skip matches inside comments (the regex picks up
        # `mangle_aot_name` mentions in `// ...` lines and
        # `/* ... */` blocks — those are not real call sites).
        if is_in_comment(text, m.start()):
            continue
        func = m.group(1)
        args_text = find_call_args(text, m.end() - 1)
        if args_text is None:
            continue
        args = split_args(args_text)
        # Strip default values: name, disambiguator, defuse_version[, env, linear]
        line_no = text[: m.start()].count("\n") + 1
        line_start = text.rfind("\n", 0, m.start()) + 1
        line_end = text.find("\n", m.start())
        if line_end < 0:
            line_end = len(text)
        full_line = text[line_start:line_end]
        prev_line_start = text.rfind("\n", 0, line_start - 1) + 1
        prev_line = text[prev_line_start : max(line_start - 1, prev_line_start)]
        # Annotation contract (#2168): same line OR previous line.
        if ALLOW_RE.search(prev_line) or ALLOW_RE.search(full_line):
            continue
        if len(args) < 5:
            # 3-arg call (defuse only) — pre-#2091 shape; flag production.
            findings.append((line_no, func, "<3-arg>", "<3-arg>"))
            continue
        env_arg = args[3]
        lin_arg = args[4]
        # Strip inline trailing comments
        env_clean = re.sub(r"//.*$", "", env_arg).strip()
        lin_clean = re.sub(r"//.*$", "", lin_arg).strip()
        # Allow if either arg is not a literal 0 (computed / live).
        env_is_zero = env_clean in ("0", "0u", "0ULL", "0ull", "0UL", "0ul", "(std::uint64_t)0")
        lin_is_zero = lin_clean in ("0", "0u", "0ULL", "0ull", "(std::uint8_t)0")
        if env_is_zero and lin_is_zero:
            findings.append((line_no, func, env_clean, lin_clean))
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true", help="Run a quick self-test against the in-tree fixture.")
    parser.add_argument("--strict", action="store_true", help="Treat any 3-arg call as a finding.")
    args = parser.parse_args()

    if args.self_test:
        # Minimal sanity: scan the bridge TU for the wired calls.
        path = SRC_DIR / "aura_jit_bridge.cpp"
        findings = scan_file(path)
        # aura_jit_bridge.cpp's generate_registration_c uses live values
        # (aot_resolve_emit_env_frame_version + emit_linear_state) — no
        # literal 0,0 calls in production paths.
        zero_zero = [f for f in findings if f[2] == "0" and f[3] == "0"]
        if zero_zero:
            print(f"SELF-TEST FAIL: literal (0,0) found in {path}: {zero_zero}")
            return 1
        print(f"SELF-TEST OK: {path} has no literal (0,0) production calls")
        return 0

    files = []
    for root, _dirs, names in os.walk(SRC_DIR):
        for n in names:
            if not n.endswith((".cpp", ".h", ".ixx", ".cc")):
                continue
            if n in SKIP_BASENAMES:
                continue
            files.append(Path(root) / n)

    total_findings = 0
    for f in sorted(files):
        findings = scan_file(f)
        for line_no, func, env_arg, lin_arg in findings:
            print(
                f"{f}:{line_no}: {func}(...) literal (env={env_arg}, lin={lin_arg}) — "
                f"thread live values via aot_resolve_emit_* or "
                f"add `# 2091-allow-zero` comment."
            )
            total_findings += 1
    if total_findings > 0:
        print(f"FAIL: {total_findings} suspicious AOT env/linear stamp sites.")
        return 1
    print(
        "OK: every production AOT emit / reemit / registration site "
        "threads live env_frame_version + linear_state (or carries "
        "an explicit `# 2091-allow-zero` annotation)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
