#!/usr/bin/env python3
"""check_mutation_guard_coverage.py — Issue #1950 cycle 1

Audit script that scans all `add("compile:*", ...)` and
`add("mutate:*", ...)` primitive registrations in
src/compiler/evaluator_primitives_*.cpp and reports which ones are
wrapped in `run_under_mutation_guard(...)` (Issue #1842/#1889
pattern) vs which ones have raw `ev.*` mutation calls without a
Guard.

Issue #1950 AC #2: "所有 compile:* / mutate:* primitives 100%
包装 Guard + StableNodeRef 验证". The dtor atomic-batching (AC
#1) was already shipped in #1747 (≤6 atomics, in-code comment at
evaluator.ixx:12588). The remaining gap is primitive-surface Guard
coverage — this linter measures the gap.

Default: non-strict (exit 0, prints coverage stats). Use
--strict to enforce (exit 1 if any uncovered primitive found —
use after cycle 1d migration closes the gap).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Primitive-registration pattern: add("compile:xxx", ...) and
# add("mutate:xxx", ...). Other prefixes (seva/eda/etc.) are
# out-of-scope for this AC.
ADD_RE = re.compile(r'add\("(compile|mutate):([a-z0-9\-_?!+]+)"')

# Patterns indicating the primitive is wrapped in a Guard.
GUARD_PATTERNS = [
    re.compile(r"run_under_mutation_guard\("),
    re.compile(r"run_compile_dirty_under_guard\("),  # compile-dirty Guard (Issue #1950)
    re.compile(r"with_compiler_service_pin\("),  # relower-strategy Guard (#1855)
    re.compile(r"MutationBoundaryGuard::try_acquire\("),
    re.compile(r"MutationBoundaryGuard\s+\w+\s*\("),  # ctor invocation
    re.compile(r"aura_orch_agent_body_try_acquire\("),  # orch body guard
]

# Patterns indicating the primitive is read-only or metadata-only
# (no Guard needed). Issue #1950 AC #2 only requires Guard on
# primitives that perform AST/IR mutation; metadata-only and
# read-only primitives are out of scope.
READONLY_PATTERNS = [
    re.compile(r"\?\s*$"),  # predicate primitives (e.g., compile:dirty?)
    re.compile(r"-stats\b"),
    re.compile(r"-count\b"),
    re.compile(r"query-"),
    re.compile(r"verify-dirty\?"),
    re.compile(r"is-"),
    re.compile(r"^set-agent-fingerprint$"),  # #1419 metadata-only
    re.compile(r"^validate-reflected$"),  # #1907 reflect metric bump only
    re.compile(r"^validate-against-schema$"),  # #1907 reflect metric bump only
    re.compile(r"^snapshot$"),  # read-only fast-path
    re.compile(r"^hw-bitvec-width$"),  # read-only type-registry lookup
    re.compile(r"^per-defuse-index-callers$"),  # read-only hash-table build
]

# Scan targets.
SCAN_DIR = REPO_ROOT / "src" / "compiler"
SCAN_GLOB = "evaluator_primitives_*.cpp"


def is_readonly(name: str) -> bool:
    return any(p.search(name) for p in READONLY_PATTERNS)


def scan_file(path: Path) -> tuple[list[tuple[int, str, bool]], list[tuple[int, str, bool]]]:
    """Return (covered, uncovered) primitive entries from the file.

    Each entry is (line_no, primitive_name, is_mutation_kind).
    """
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return [], []
    covered: list[tuple[int, str, bool]] = []
    uncovered: list[tuple[int, str, bool]] = []
    # Walk line-by-line; for each add() primitive, look at the
    # surrounding ~30 lines for Guard-wrapping patterns.
    lines = text.splitlines()
    for i, line in enumerate(lines, start=1):
        m = ADD_RE.search(line)
        if not m:
            continue
        kind, name = m.group(1), m.group(2)
        is_mutation_kind = kind == "mutate"
        # Read-only / metadata-only primitives don't need a Guard.
        # Applies to BOTH compile:* AND mutate:* (some mutate:* are
        # metadata-only per #1419 / #1907 documented exceptions).
        if is_readonly(name):
            covered.append((i, f"{kind}:{name}", is_mutation_kind))
            continue
        # Look at the next ~80 lines for Guard wrapping (some primitives
        # have lengthy capability gates / arg validation before the Guard
        # call; the run_compile_dirty_under_guard wrap can sit 40+
        # lines below the add()).
        window_end = min(len(lines), i + 80)
        window = "\n".join(lines[i - 1 : window_end])
        has_guard = any(p.search(window) for p in GUARD_PATTERNS)
        if has_guard:
            covered.append((i, f"{kind}:{name}", is_mutation_kind))
        else:
            uncovered.append((i, f"{kind}:{name}", is_mutation_kind))
    return covered, uncovered


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--strict", action="store_true", help="Exit 1 if any uncovered primitive found.")
    ap.add_argument("--quiet", action="store_true", help="Only print summary line.")
    args = ap.parse_args()

    files = sorted(SCAN_DIR.glob(SCAN_GLOB))
    if not files:
        print(f"ERROR: no files matched {SCAN_DIR}/{SCAN_GLOB}", file=sys.stderr)
        return 1

    total_covered = 0
    total_uncovered = 0
    per_file: list[tuple[Path, int, int, list[tuple[int, str, bool]]]] = []
    for path in files:
        covered, uncovered = scan_file(path)
        total_covered += len(covered)
        total_uncovered += len(uncovered)
        per_file.append((path, len(covered), len(uncovered), uncovered))

    total = total_covered + total_uncovered
    pct = (100.0 * total_covered / total) if total > 0 else 0.0

    if args.quiet:
        print(
            f"mutation_guard_coverage: {total_covered}/{total} covered ({pct:.1f}%) — "
            f"{total_uncovered} uncovered across {len(per_file)} files (Issue #1950)"
        )
        return 1 if (args.strict and total_uncovered > 0) else 0

    print("Issue #1950 mutation_guard_coverage report:")
    print(f"  total primitives scanned : {total}")
    print(f"  covered                  : {total_covered} ({pct:.1f}%)")
    print(f"  uncovered                : {total_uncovered}")
    print()
    for path, covered, uncovered, _unc_list in per_file:
        rel = path.relative_to(REPO_ROOT)
        print(f"  {rel}: covered={covered}, uncovered={uncovered}")
    if total_uncovered > 0:
        print()
        print(f"  Uncovered primitives ({total_uncovered} total):")
        shown = 0
        for path, _, _, unc_list in per_file:
            for lineno, name, is_mut in unc_list:
                if shown >= 30:
                    print(f"  ... and {total_uncovered - 30} more")
                    break
                kind = "mut" if is_mut else "cpl"
                print(f"    {kind} {name}  ({path.name}:{lineno})")
                shown += 1
            if shown >= 30:
                break

    if args.strict and total_uncovered > 0:
        print(
            f"\nFAIL: {total_uncovered} uncovered primitive(s). Use --strict after migration closes the gap.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
