#!/usr/bin/env python3
"""check_workspace_epoch_migration.py — Issue #2039 / #1964 cycle 2d gate

Final ownership model (documented in src/core/workspace_epoch.hh):

| Kind       | Storage owner                      | Status after 2d          |
|------------|------------------------------------|--------------------------|
| Mutation   | process-global WorkspaceEpoch only | migrated; no dual field  |
| Bridge     | WorkspaceEpoch + C dual-write      | migrated; C is mirror    |
| Generation | per-FlatAST generation_            | intentional (not global) |
| Wrap       | per-FlatAST wrap_epoch_            | intentional              |
| Subtree    | per-FlatAST subtree_gen_           | intentional              |
| node_gen_  | per-FlatAST                        | intentional              |

This linter enforces that process-global Mutation storage is NOT dual-
declared outside WorkspaceEpoch. Per-AST fields, per-closure stamped
bridge_epoch, metric names, and C dual-write mirrors are allowed.

Forbidden (production src/ only):
  - `mutation_epoch_.load/store/fetch_add/exchange` field ops
  - `std::atomic<...> mutation_epoch_{...}` field declarations
  - bare `mutation_epoch_{0}` member initializers on CompilerService

Allowed (not flagged):
  - comments (stripped before scan)
  - workspace_epoch.hh itself
  - tests/**
  - per-AST generation_ / wrap_epoch_ / subtree_gen_ / node_gen_
  - ClosureBridgeData.bridge_epoch stamps, metrics, hook names
  - g_current_bridge_epoch C dual-write in aura_jit_bridge / runtime / stubs

Ref: Issue #2039 AC1 (linter clean under final ownership model).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Forbidden dual-storage patterns for process-global Mutation epoch.
# After #2039, sole storage is WorkspaceEpoch::Mutation.
FORBIDDEN_PATTERNS: list[tuple[str, re.Pattern[str]]] = [
    (
        "mutation_epoch_field_op",
        re.compile(
            r"\bmutation_epoch_\.(?:load|store|fetch_add|exchange|"
            r"compare_exchange_weak|compare_exchange_strong)\b"
        ),
    ),
    (
        "mutation_epoch_atomic_decl",
        re.compile(r"\bstd::atomic\s*<[^>;]*>\s*mutation_epoch_"),
    ),
    (
        "mutation_epoch_field_init",
        re.compile(r"\bmutation_epoch_\{"),
    ),
]

# Files where the (deleted) dual field historically lived; still
# scanned for regressions. The shim itself is the only allowed owner
# of process-global mutation storage APIs.
ALLOWED_FILES: set[str] = {
    "src/core/workspace_epoch.hh",
    "scripts/check_workspace_epoch_migration.py",
}

SCAN_DIRS = ["src/core", "src/compiler", "src/serve", "src/repl", "src/reflect"]
SCAN_EXTS = {".cpp", ".h", ".hpp", ".hh", ".ixx", ".cppm"}


def strip_comments_and_strings(src: str) -> str:
    """Replace comments and string literals with whitespace of equal length."""
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
    """Return list of (line_no, pattern_name, matched_text) violations."""
    rel = path.relative_to(REPO_ROOT).as_posix()
    if rel in ALLOWED_FILES:
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
        for pname, pat in FORBIDDEN_PATTERNS:
            for m in pat.finditer(stripped_line):
                violations.append((lineno, pname, m.group(0)))
    return violations


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument(
        "--self-test",
        action="store_true",
        help="Run linter against a fixture; exit 0 iff clean.",
    )
    ap.add_argument(
        "--quiet",
        action="store_true",
        help="Only print summary line + exit code; no per-violation detail.",
    )
    ap.add_argument(
        "--strict",
        action="store_true",
        help="Exit 1 if any dual-storage violations found (CI gate after #2039).",
    )
    ap.add_argument(
        "--per-violation",
        action="store_true",
        help="Print every violation (default: per-file first 20 + total).",
    )
    args = ap.parse_args()

    if args.self_test:
        fixture_violations = scan_file(Path(__file__))
        if fixture_violations:
            print("SELF-TEST FAILED — allowed file produced violations:")
            for v in fixture_violations:
                print(f"  {v}")
            return 1
        # Positive fixture: a synthetic dual-storage line must match.
        synthetic = "    std::atomic<std::uint64_t> mutation_epoch_{0};\n"
        synthetic += "    auto x = mutation_epoch_.load(std::memory_order_acquire);\n"
        stripped = strip_comments_and_strings(synthetic)
        hits = 0
        for line in stripped.splitlines():
            for _pname, pat in FORBIDDEN_PATTERNS:
                hits += len(list(pat.finditer(line)))
        if hits < 2:
            print(f"SELF-TEST FAILED — synthetic dual-storage only hit {hits} patterns")
            return 1
        print("SELF-TEST OK — linter scans itself cleanly; forbids dual mutation_epoch_.")
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

    if args.quiet:
        print(
            f"workspace_epoch_migration: {total_violations} violation(s) "
            f"across {len(violation_files)}/{total_files} files"
        )
        return 0 if total_violations == 0 else 1

    if total_violations == 0:
        print(
            f"✓ workspace_epoch_migration: clean — 0 dual-storage violations "
            f"across {total_files} files (Issue #2039 cycle 2d)"
        )
        return 0

    print(
        f"✗ workspace_epoch_migration: {total_violations} dual-storage violation(s) "
        f"across {len(violation_files)}/{total_files} files "
        f"(Issue #2039 — Mutation must live only in WorkspaceEpoch)\n"
        f"  Forbidden: mutation_epoch_ field ops/decls outside workspace_epoch.hh\n"
        f"  Allowed: per-FlatAST generation_/wrap_epoch_/subtree_gen_/node_gen_, "
        f"C dual-write g_current_bridge_epoch, metrics, stamped bridge_epoch"
    )
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
    # Non-strict still reports but exits 0 only when clean under 2d.
    # After #2039, default matches --strict for CI simplicity.
    return 1 if total_violations else 0


if __name__ == "__main__":
    sys.exit(main())
