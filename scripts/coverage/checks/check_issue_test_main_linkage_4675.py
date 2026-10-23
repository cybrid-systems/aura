#!/usr/bin/env python3
"""Issue #4675: an issue-test binary whose `int main()` sits INSIDE the
anonymous namespace has internal linkage, so the linker never sees a
`main` symbol and the whole ninja build stops.

  CI symptom
      [1800/1810] Linking CXX executable test_hash_empty_string_key
      FAILED: [code=1] test_hash_empty_string_key
      mold: error: undefined symbol: main
      ninja: build stopped: cannot make progress due to previous errors.

  Mechanism
      A file-scope `namespace { ... }` gives every name in it INTERNAL
      linkage. `int main()` inside that namespace is therefore not the
      program entry point the C runtime looks up, and the link fails with
      `undefined symbol: main`. The sibling issue tests all close the
      namespace BEFORE main:
          test_hash_table_grow.cpp  } // namespace @480, int main() @527
          test_hash_probe.cpp       } // namespace @307, int main() @322

  What this gate does
      Scans every C++ source under tests/ for an `int main(` that is
      lexically inside an anonymous namespace and rejects it. A single
      mistake silently drops that binary from the CI matrix (and, because
      ninja stops on the link error, every LATER target in the job too),
      so the check is worth more than the one-line move.

  AC1  no test source declares `int main()` inside an anonymous namespace
  AC2  the #4675 test file closes its namespace before `int main()`
  AC3  the sibling issue tests keep the same close-before-main shape
       (the pattern the fix copies)

Usage:
  python3 scripts/coverage/checks/check_issue_test_main_linkage_4675.py
  python3 scripts/coverage/checks/check_issue_test_main_linkage_4675.py --self-test
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# The test that regressed in CI (the file the #4675 fix touches).
TARGET_REL = "tests/compiler/test_hash_empty_string_key.cpp"
# Sibling issue tests that already use the close-before-main shape.
SIBLINGS = (
    "tests/compiler/test_hash_table_grow.cpp",
    "tests/compiler/test_hash_probe.cpp",
)

MAIN_RE = re.compile(r"\bint\s+main\s*\(")
# `namespace {` opens an anonymous namespace. A NAMED namespace
# (`namespace foo {`) does NOT give internal linkage, so it is excluded.
ANON_OPEN_RE = re.compile(r"\bnamespace\s*\{")
NAMED_OPEN_RE = re.compile(r"\bnamespace\s+[A-Za-z_][A-Za-z0-9_]*\s*\{")


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _strip_noise(text: str) -> str:
    """Blank out comments and string/char literals, keeping offsets.

    The scanner works on character offsets, so the removed spans are
    replaced with spaces rather than deleted - every index still points
    at the same character in the original file.
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                out[i] = " "
                i += 1
            for _ in range(2):
                if i < n:
                    out[i] = " "
                    i += 1
        elif c in ('"', "'"):
            quote = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    out[i] = " "
                    if i + 1 < n:
                        out[i + 1] = " "
                    i += 2
                    continue
                if text[i] == quote:
                    break
                out[i] = " "
                i += 1
            i += 1
        else:
            i += 1
    return "".join(out)


def _anon_namespace_spans(text: str) -> list[tuple[int, int]]:
    """Character spans of anonymous-namespace bodies in `text`.

    Each anonymous `namespace {` is matched with its closing brace by
    brace counting, so a nested named namespace inside it does not end
    the span early and a nested anonymous namespace is handled
    independently.
    """
    clean = _strip_noise(text)
    spans: list[tuple[int, int]] = []
    for m in ANON_OPEN_RE.finditer(clean):
        # A NAMED namespace (`namespace foo {`) is not an anonymous one;
        # the anonymous regex cannot match it, but a named namespace
        # spelled `namespace {` never is, so nothing to exclude here.
        depth = 0
        started = False
        for j in range(m.end() - 1, len(clean)):
            ch = clean[j]
            if ch == "{":
                depth += 1
                started = True
            elif ch == "}":
                depth -= 1
                if started and depth == 0:
                    spans.append((m.start(), j))
                    break
    return spans


def _line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def find_main_in_anon_namespace(text: str) -> list[int]:
    """Line numbers of every `int main(` inside an anonymous namespace."""
    clean = _strip_noise(text)
    spans = _anon_namespace_spans(clean)
    hits: list[int] = []
    for m in MAIN_RE.finditer(clean):
        for start, end in spans:
            if start < m.start() < end:
                hits.append(_line_of(text, m.start()))
                break
    return hits


def _main_is_global(text: str) -> bool:
    """True when `int main(` is NOT inside any anonymous namespace."""
    return not find_main_in_anon_namespace(text)


def _closes_namespace_before_main(text: str) -> bool:
    """True when the anonymous namespace is closed before `int main(`.

    Derived from the source rather than hard-coded line numbers, so the
    check keeps working when the file is edited.
    """
    clean = _strip_noise(text)
    spans = _anon_namespace_spans(clean)
    if not spans:
        return False
    mains = list(MAIN_RE.finditer(clean))
    if not mains:
        return False
    first_main = mains[0]
    # The close of the anonymous namespace that starts before main.
    return all(not start < first_main.start() < end for start, end in spans)


def check_tests_tree() -> list[str]:
    """AC1: no test source has `int main()` inside an anonymous namespace."""
    fails: list[str] = []
    tests_dir = ROOT / "tests"
    if not tests_dir.is_dir():
        return ["AC1: the tests/ directory is missing - cannot scan the issue tests"]
    for p in sorted(tests_dir.rglob("*.cpp")):
        text = p.read_text(encoding="utf-8", errors="replace")
        for line in find_main_in_anon_namespace(text):
            rel = p.relative_to(ROOT)
            fails.append(
                f"AC1: {rel}:{line} declares int main() INSIDE an anonymous namespace - "
                "it has internal linkage, so the link fails with 'undefined symbol: main' "
                "and the whole ninja build stops (#4675)"
            )
    return fails


def check_target() -> list[str]:
    """AC2: the #4675 test file closes its namespace before `int main()`."""
    text = _read(TARGET_REL)
    if not text:
        return [f"AC2: {TARGET_REL} is missing - the #4675 fix cannot be verified"]
    fails: list[str] = []
    if not _main_is_global(text):
        for line in find_main_in_anon_namespace(text):
            fails.append(
                f"AC2: {TARGET_REL}:{line} still declares int main() inside the anonymous "
                "namespace - close the namespace before main (#4675)"
            )
    if not _closes_namespace_before_main(text):
        fails.append(f"AC2: {TARGET_REL} does not close its anonymous namespace before int main() (#4675)")
    return fails


def check_siblings() -> list[str]:
    """AC3: the sibling issue tests keep the close-before-main shape."""
    fails: list[str] = []
    for rel in SIBLINGS:
        text = _read(rel)
        if not text:
            continue
        if not _main_is_global(text):
            fails.append(
                f"AC3: {rel} declares int main() inside an anonymous namespace - "
                "the sibling pattern the #4675 fix copies is broken"
            )
    return fails


def _self_test() -> int:
    bad = """
namespace {
int g_passed = 0;
int main() { return 0; }
} // namespace
"""
    good = """
namespace {
int g_passed = 0;
} // namespace
int main() { return 0; }
"""
    named = """
namespace helper {
int f() { return 1; }
}
int main() { return 0; }
"""
    cases = [
        ("main inside the anonymous namespace is caught", find_main_in_anon_namespace(bad) == [4]),
        ("main after the namespace close is not caught", find_main_in_anon_namespace(good) == []),
        ("a NAMED namespace does not trip the check", find_main_in_anon_namespace(named) == []),
        ("the close-before-main shape is detected", _closes_namespace_before_main(good)),
        ("the broken shape is rejected", not _closes_namespace_before_main(bad)),
    ]
    for label, ok in cases:
        if not ok:
            print(f"FAIL #4675 self-test: {label}", file=sys.stderr)
            return 1
    print("OK #4675 self-test: the anonymous-namespace main scan catches the regression")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Issue #4675 issue-test main linkage gate")
    ap.add_argument("--self-test", action="store_true", help="run the built-in probes")
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test()

    fails = check_tests_tree() + check_target() + check_siblings()
    if fails:
        print("FAIL #4675 issue_test_main_linkage:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1

    print("OK #4675 issue_test_main_linkage: no issue test declares int main() inside an anonymous namespace")
    return 0


if __name__ == "__main__":
    sys.exit(main())
