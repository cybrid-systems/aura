#!/usr/bin/env python3
"""check_orch_mvp_scope.py — Issue #1965 / #1966 orch MVP-scope linter

After Issue #1966 the orch/ public surface is single-agent MVP only.
The multi-agent symbols that were temporarily // DEFERRED under #1965
cycle 1 have been **removed** from orch headers:

  - AgentRegistry / global_agent_registry  → evaluator-local OrchAgentNameTable
    (orch:spawn-agent / orch:agent-join name bookkeeping only)
  - conduct_parallel                      → use serve::parallel_orch::parallel_intend

This linter is a reintroduction guard: any non-comment / non-string
occurrence of the removed identifiers outside the allowlist fails
under --strict (wired into ./build.py gate).

MVP (allowed freely):
  spawn_agent_with_mailbox, join_agent, join_agents, agent_send,
  agent_recv, AgentHandle, AgentSpec, OrchModuleStats,
  release_agent_memory_reservation, parallel_intend (re-export)

Ref: Issue #1965 AC #1, Issue #1966 (remove deferred multi-agent public surface).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Removed multi-agent public identifiers (must not reappear in production code).
REMOVED_PATTERNS: list[tuple[str, re.Pattern[str]]] = [
    ("AgentRegistry", re.compile(r"\bAgentRegistry\b")),
    ("global_agent_registry", re.compile(r"\bglobal_agent_registry\b")),
    ("conduct_parallel", re.compile(r"\bconduct_parallel\b")),
]

# Files allowed to mention removed identifiers (docs/rationale only, or
# the linter itself). After #1966 no production/test code may call them.
ALLOWLIST: set[str] = {
    "scripts/check_orch_mvp_scope.py",
}

# Directories to scan (production + tests).
SCAN_DIRS = ["src/orch", "src/compiler", "src/serve", "src/repl", "src/reflect", "tests"]
SCAN_EXTS = {".cpp", ".h", ".hpp", ".hh", ".ixx", ".cppm"}


def strip_comments_and_strings(src: str) -> str:
    """Replace comments and string literals with whitespace of equal length.
    Best-effort: handles // line comments, /* */ block comments, and "..."
    / '...' string literals (no raw strings, no template-arg edge cases —
    those are rare for orch symbol names)."""
    out = list(src)
    i = 0
    n = len(src)
    in_block = False
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if in_block:
            if c == "*" and nxt == "/":
                out[i] = " "
                out[i + 1] = " "
                in_block = False
                i += 2
                continue
            if c != "\n":
                out[i] = " "
            i += 1
            continue
        if c == "/" and nxt == "/":
            j = i
            while j < n and src[j] != "\n":
                out[j] = " "
                j += 1
            i = j
            continue
        if c == "/" and nxt == "*":
            out[i] = " "
            out[i + 1] = " "
            in_block = True
            i += 2
            continue
        if c == '"':
            j = i + 1
            out[i] = " "
            while j < n and src[j] != '"':
                if src[j] == "\\" and j + 1 < n:
                    out[j] = " "
                    out[j + 1] = " "
                    j += 2
                else:
                    if src[j] != "\n":
                        out[j] = " "
                    j += 1
            if j < n:
                out[j] = " "
            i = j + 1
            continue
        if c == "'":
            j = i + 1
            out[i] = " "
            while j < n and src[j] != "'":
                if src[j] == "\\" and j + 1 < n:
                    out[j] = " "
                    out[j + 1] = " "
                    j += 2
                else:
                    if src[j] != "\n":
                        out[j] = " "
                    j += 1
            if j < n:
                out[j] = " "
            i = j + 1
            continue
        i += 1
    return "".join(out)


def scan_file(path: Path) -> list[tuple[int, str, str]]:
    rel = path.relative_to(REPO_ROOT).as_posix()
    if rel in ALLOWLIST:
        return []
    try:
        raw = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    stripped = strip_comments_and_strings(raw)
    raw_lines = raw.splitlines()
    stripped_lines = stripped.splitlines()
    violations: list[tuple[int, str, str]] = []
    for lineno, (_raw_line, stripped_line) in enumerate(zip(raw_lines, stripped_lines, strict=False), start=1):
        for pname, pat in REMOVED_PATTERNS:
            for m in pat.finditer(stripped_line):
                violations.append((lineno, pname, m.group(0)))
    return violations


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument(
        "--self-test",
        action="store_true",
        help="Run linter against itself; exit 0 iff allowlisted clean.",
    )
    ap.add_argument(
        "--quiet",
        action="store_true",
        help="Only print summary line + exit code; no per-violation detail.",
    )
    ap.add_argument(
        "--strict",
        action="store_true",
        help="Exit 1 if any reintroduction of removed multi-agent symbols is found. "
        "Default after #1966 is still report-only unless --strict (gate always uses --strict).",
    )
    ap.add_argument(
        "--per-violation",
        action="store_true",
        help="Print every violation (default: per-file first 5 + total).",
    )
    args = ap.parse_args()

    if args.self_test:
        fixture_violations = scan_file(Path(__file__))
        if fixture_violations:
            print("SELF-TEST FAILED — allowlisted file produced violations:")
            for v in fixture_violations:
                print(f"  {v}")
            return 1
        print("SELF-TEST OK — linter scans itself cleanly (allowlisted file).")
        return 0

    total_files = 0
    total_violations = 0
    violation_files: list[tuple[Path, list[tuple[int, str, str]]]] = []
    for d in SCAN_DIRS:
        dirpath = REPO_ROOT / d
        if not dirpath.exists():
            continue
        for path in sorted(dirpath.rglob("*")):
            if not path.is_file():
                continue
            if path.suffix not in SCAN_EXTS:
                continue
            total_files += 1
            v = scan_file(path)
            if v:
                total_violations += len(v)
                violation_files.append((path, v))

    if total_violations == 0:
        print(f"✓ orch_mvp_scope: clean — 0 removed-symbol reintroductions across {total_files} files (Issue #1966)")
        return 0

    print(
        f"⚠ orch_mvp_scope: {total_violations} removed multi-agent symbol(s) "
        f"reintroduced across {len(violation_files)}/{total_files} files "
        f"(Issue #1966 — AgentRegistry / global_agent_registry / conduct_parallel "
        f"must not return to the public orch surface)\n"
        f"  Allowlisted files ({len(ALLOWLIST)}): see ALLOWLIST in "
        f"scripts/check_orch_mvp_scope.py\n"
        f"  Use --strict to enforce (exit 1; ./build.py gate always uses --strict)."
    )
    if not args.quiet:
        if args.per_violation:
            for path, vs in violation_files:
                rel = path.relative_to(REPO_ROOT).as_posix()
                print(f"  {rel}:")
                for lineno, pname, matched in vs:
                    print(f"    L{lineno}  {pname}  match='{matched}'")
        else:
            for path, vs in violation_files[:20]:
                rel = path.relative_to(REPO_ROOT).as_posix()
                print(f"  {rel}: {len(vs)} violation(s)")
            if len(violation_files) > 20:
                print(f"  ... and {len(violation_files) - 20} more files (use --per-violation for full detail)")

    if args.strict:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
