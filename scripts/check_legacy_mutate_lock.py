#!/usr/bin/env python3
"""Issue #1904: legacy mutate:* primitive lock + bump gate.

Prevents new `std::unique_lock<std::shared_mutex>(ev.workspace_mtx_)` +
explicit `ev.defuse_version_.fetch_add(...)` patterns in
src/compiler/evaluator_primitives_*.cpp. The #1904 migration moves
every such site to MutationBoundaryGuard RAII (try_acquire or legacy
ctor) so the Guard owns the lock + version bump + rollback
invariant. This linter enforces that the migration is complete:

  - Zero `std::unique_lock<std::shared_mutex>(... ev.workspace_mtx_ ...)`
    in src/compiler/evaluator_primitives_*.cpp (the manual lock pattern)
  - Zero `ev.defuse_version_.fetch_add(...)` outside MutationBoundaryGuard
    context (the manual version bump pattern)
  - Allowlisted sites (test fixtures + introspection helpers) are
    skipped via the in-file `// #1904-allow legacy-lock` marker

Exit 0 = OK, 1 = legacy-pattern violation found.

Usage:
  python3 scripts/check_legacy_mutate_lock.py
  python3 scripts/check_legacy_mutate_lock.py --self-test
  python3 scripts/check_legacy_mutate_lock.py --files src/compiler/foo.cpp
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def strip_cpp_comments(text: str) -> str:
    """Remove C++ line and block comments so the linter does not flag
    documentation that mentions the legacy pattern."""
    # Block comments first (they may contain newlines).
    text = re.sub(r"/\*[\s\S]*?\*/", "", text)
    # Line comments (// to end of line; mind the // inside string literals
    # is rare in the affected files since we only scan evaluator_primitives_*).
    text = re.sub(r"//[^\n]*", "", text)
    return text


ROOT = Path(__file__).resolve().parents[1]
PRIM_GLOB = re.compile(r"^src/compiler/evaluator_primitives.*\.cpp$")

# Manual lock pattern: std::unique_lock<std::shared_mutex> ... workspace_mtx_
# Captures the variable name (e.g. `wlock`) so we can confirm it actually
# guards workspace_mtx_.
LEGACY_LOCK_RE = re.compile(r"std::unique_lock<std::shared_mutex>\s+\w+\s*\(\s*ev\.workspace_mtx_")

# Manual version bump pattern: ev.defuse_version_.fetch_add(...)
LEGACY_BUMP_RE = re.compile(r"ev\.defuse_version_\.fetch_add\s*\(")

# In-file marker to opt-out of the linter for a specific line.
# Use sparingly; each marker requires a comment explaining why.
ALLOW_MARKER = "#1904-allow legacy-lock"


def collect_files(args_files: list[str] | None) -> list[Path]:
    if args_files:
        return [Path(f) for f in args_files if PRIM_GLOB.match(f)]
    return sorted(p for p in ROOT.glob("src/compiler/evaluator_primitives*.cpp"))


def scan_file(path: Path) -> list[tuple[int, str, str]]:
    """Return list of (line_no, line_text, rule) for each violation."""
    out: list[tuple[int, str, str]] = []
    raw = path.read_text(encoding="utf-8")
    text = strip_cpp_comments(raw)
    # Build a line index: for each line in raw, find its counterpart in
    # the stripped text (1:1 line numbering preserved because strip
    # only removes inline content, never whole lines).
    for i, (orig_line, stripped_line) in enumerate(zip(raw.splitlines(), text.splitlines(), strict=False), start=1):
        if ALLOW_MARKER in orig_line:
            continue
        if LEGACY_LOCK_RE.search(stripped_line):
            out.append((i, orig_line.rstrip(), "legacy-lock"))
        elif LEGACY_BUMP_RE.search(stripped_line):
            out.append((i, orig_line.rstrip(), "legacy-bump"))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--files", nargs="*", help="Restrict scan to these files")
    ap.add_argument("--self-test", action="store_true", help="Run self-test fixtures")
    args = ap.parse_args()

    if args.self_test:
        return _self_test()

    files = collect_files(args.files)
    if not files:
        print("no files to scan (check glob)", file=sys.stderr)
        return 1

    total_violations = 0
    by_rule: dict[str, int] = {}
    for path in files:
        rel = path.relative_to(ROOT)
        for line_no, line_text, rule in scan_file(path):
            total_violations += 1
            by_rule[rule] = by_rule.get(rule, 0) + 1
            print(f"{rel}:{line_no}: [{rule}] {line_text.strip()}", file=sys.stderr)

    if total_violations > 0:
        print(
            f"\nFAIL: {total_violations} legacy mutate:* pattern(s) remain "
            f"(rules: {by_rule}). Issue #1904 mandate: migrate every "
            f"manual lock + bump site to MutationBoundaryGuard RAII.",
            file=sys.stderr,
        )
        return 1
    print(f"OK: scanned {len(files)} file(s), zero legacy mutate:* patterns remain.")
    return 0


def _self_test() -> int:
    """Verify the regex patterns with known-good + known-bad fixtures."""
    cases = [
        # (line, expected_rule_or_none)
        ("std::unique_lock<std::shared_mutex> wlock(ev.workspace_mtx_);", "legacy-lock"),
        ("std::unique_lock<std::shared_mutex> wl(ev.workspace_mtx_);", "legacy-lock"),
        ("    std::unique_lock<std::shared_mutex> wlock(ev.workspace_mtx_);", "legacy-lock"),
        ("ev.defuse_version_.fetch_add(1, std::memory_order_acq_rel);", "legacy-bump"),
        ("ev.defuse_version_.fetch_add(1);", "legacy-bump"),
        # Negative: should NOT match (use a different mutex or are inside Guard)
        ("std::unique_lock<std::shared_mutex> wlock(ev.some_other_mtx_);", None),
        ("ev.defuse_version_snapshot();", None),
        ("// #1904-allow legacy-lock: legacy fixture for test_issue_xxx", None),
        # Real Guard ctor uses lock_(ev.workspace_mtx_, std::defer_lock) - different pattern
        ("lock_(ev.workspace_mtx_, std::defer_lock)", None),
    ]
    failures = 0
    for line, expected in cases:
        matched_rule = None
        if LEGACY_LOCK_RE.search(line):
            matched_rule = "legacy-lock"
        elif LEGACY_BUMP_RE.search(line):
            matched_rule = "legacy-bump"
        if matched_rule != expected:
            print(f"FAIL: line={line!r} expected={expected} got={matched_rule}", file=sys.stderr)
            failures += 1
    if failures:
        print(f"\n{failures} self-test failure(s)", file=sys.stderr)
        return 1
    print(f"OK: self-test passed ({len(cases)} fixtures)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
