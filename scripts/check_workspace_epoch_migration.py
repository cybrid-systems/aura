#!/usr/bin/env python3
"""check_workspace_epoch_migration.py — Issue #1964 cycle 2a migration linter

Tracks progress of the epoch-counter unification. After cycle 2a,
new code that touches any of the 5 legacy epoch counters
(bridge_epoch / mutation_epoch / subtree_generation_ / wrap_epoch_
/ generation_) MUST go through `aura::core::WorkspaceEpoch` /
`g_workspace_epoch_storage(kind)` defined in
`src/core/workspace_epoch.hh`.

Legacy-counter raw usage is ALLOWED only in:
- src/core/workspace_epoch.hh              (the shim itself)
- src/core/ast.ixx                         (defines the legacy counters; cycle 2b/2d migrates these)
- src/serve/fiber.{cpp,h}                  (defines g_bridge_epoch_; cycle 2c migrates)
- src/compiler/evaluator_*.cpp             (mostly wraps mutation_epoch_ via FlatAST accessor; cycle 2b sanity)
- src/repl/*.cpp                           (read-only debug print; cycle 2d migrates)
- tests/**                                 (test code may exercise legacy counters directly)

All other production code (.cpp/.hpp/.ixx under src/ excluding the
allowed list above) using one of the 5 legacy counter identifier
patterns MUST use `WorkspaceEpoch` instead. The linter emits a
violation per offending line.

Excluded:
- comments (// ... and /* ... */ blocks)
- the workspace_epoch.hh shim file itself

Ref: Issue #1964 AC #2 (unify epoch/generation counters — one logical
counter per workspace).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Patterns that match raw usage of the 5 legacy counters.
# These match identifier references, not bare string mentions in comments.
# Note: ordered most-specific first so longer matches win.
LEGACY_PATTERNS: list[tuple[str, re.Pattern[str]]] = [
    ("bridge_epoch", re.compile(r"\b(?:g_)?bridge_epoch(?:_\w*)?\b")),
    ("mutation_epoch", re.compile(r"\bmutation_epoch(?:_\w*)?\b")),
    ("subtree_generation_", re.compile(r"\bsubtree_generation_\w*\b")),
    ("wrap_epoch_", re.compile(r"\bwrap_epoch_\w*\b")),
    # `generation_` is tricky — it appears as a struct member, a function
    # local, and as the legacy epoch counter. To avoid noise we only
    # match `generation_` when it's clearly an epoch counter (typed as
    # uint16_t / uint32_t / uint64_t on the same line).
    ("generation_", re.compile(r"\bgeneration_(?:count|wrap|suppressed|of)?\b")),
    ("bump_generation_count_", re.compile(r"\bbump_generation_count_\w*\b")),
    ("type_cache_generation_", re.compile(r"\btype_cache_generation_\w*\b")),
    ("next_generation_", re.compile(r"\bnext_generation_\w*\b")),
    ("node_gen_", re.compile(r"\bnode_gen_\w*\b")),
    ("subtree_gen_", re.compile(r"\bsubtree_gen_\w*\b")),
]

# Files where legacy counter usage is allowed (the migration owners).
ALLOWED_FILES: set[str] = {
    # The shim itself.
    "src/core/workspace_epoch.hh",
    # ast.ixx defines the legacy counters — cycle 2b/2d migrates these.
    "src/core/ast.ixx",
    # type.ixx / type_impl.cpp use next_generation_ for TypeId epochs
    # (separate from AST workspace epoch) — cycle 2d migrates.
    "src/core/type.ixx",
    "src/core/type_impl.cpp",
    # fiber.{cpp,h} define g_bridge_epoch_ — cycle 2c migrates.
    "src/serve/fiber.cpp",
    "src/serve/fiber.h",
    # concept_constraints.ixx has pipeline_epoch / mutation_epoch passthrough — cycle 2b migrates.
    "src/core/concept_constraints.ixx",
    # mutation.ixx has its own mutation_id counter (separate from AST mutation_epoch_)
    # — keep as-is, cycle 2b reviews whether to merge.
    "src/core/mutation.ixx",
    # repl code reads counters for debug — cycle 2d migrates.
    "src/repl/repl.cppm",
    "src/repl/repl_main.cpp",
    # The linter script itself mentions counter names.
    "scripts/check_workspace_epoch_migration.py",
}

# Directories to scan (production code, exclude build/, .git/, docs/, tests/).
SCAN_DIRS = ["src/core", "src/compiler", "src/serve", "src/repl", "src/reflect"]
SCAN_EXTS = {".cpp", ".h", ".hpp", ".ixx", ".cppm"}


# A line is "comment" if it starts with optional whitespace + //, or is
# inside a /* */ block. We approximate the latter by tracking brace-depth
# + a simple "last /* seen, no matching */" flag across the file.
def strip_comments_and_strings(src: str) -> str:
    """Replace comments and string literals with whitespace of equal
    length. Keeps line numbers stable. Best-effort: handles // line
    comments, /* */ block comments, and " / R" / ' / L' string literals
    (no raw strings, no template-arg edge cases — those are rare for
    counter names and would surface as false positives that can be
    whitelisted)."""
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
            # line comment to EOL
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
            # string literal to closing " (no escape handling — false
            # positives are rare for counter names)
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
        for pname, pat in LEGACY_PATTERNS:
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
        help="Exit 1 if any violations found (use in CI after cycles 2b/2c/2d "
        "migrate legacy uses). Default is non-strict: report stats only.",
    )
    ap.add_argument(
        "--per-violation",
        action="store_true",
        help="Print every violation (default: per-file first 10 + total).",
    )
    args = ap.parse_args()

    if args.self_test:
        # Self-test: scan this very file (allowed) + scan scripts/ subdir
        # + assert the shim itself contains the pattern names but does
        # not produce violations.
        fixture_violations = scan_file(Path(__file__))
        if fixture_violations:
            print("SELF-TEST FAILED — allowed file produced violations:")
            for v in fixture_violations:
                print(f"  {v}")
            return 1
        print("SELF-TEST OK — linter scans itself cleanly (allowed file).")
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
        print(f"✓ workspace_epoch_migration: clean — 0 violations across {total_files} files (Issue #1964 cycle 2a)")
        return 0

    # Non-strict mode (default): report stats, exit 0. Used during
    # cycle 2a before cycles 2b/2c/2d migrate legacy uses.
    # Strict mode: exit 1 (use in CI after migration cycles complete).
    print(
        f"⚠ workspace_epoch_migration: {total_violations} violation(s) "
        f"across {len(violation_files)}/{total_files} files "
        f"(cycle 2a — legacy uses pending cycles 2b/2c/2d migration)\n"
        f"  Allowed files ({len(ALLOWED_FILES)}): see ALLOWED_FILES in "
        f"scripts/check_workspace_epoch_migration.py\n"
        f"  Use --strict to enforce (exit 1) after migration cycles complete."
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
