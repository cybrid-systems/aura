#!/usr/bin/env python3
"""check_mutation_guard_coverage.py — Issue #1950 / #1931 / #1953 / #2124

Audit script that scans all `add("compile:*", ...)` and
`add("mutate:*", ...)` primitive registrations in
src/compiler/evaluator_primitives_*.cpp and reports which ones are
wrapped in production Guard entry (`try_acquire` / helpers) vs which
ones lack a Guard.

Issue #2124: production must use MutationBoundaryGuard::try_acquire
(or run_under_mutation_guard / run_compile_dirty_under_guard /
orch body acquire). Legacy `MutationBoundaryGuard guard(ev, &ok)`
ctors are residual technical debt — --strict fails if any remain
outside test/docs comments.

Default: non-strict (exit 0, prints coverage stats). Use
--strict to enforce (exit 1 if uncovered primitives OR residual
legacy ctors — required for production Agent backpressure).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Issue #2124: also match add_mutate("mutate:…") (security-gated register).
ADD_RE = re.compile(r'(?:add|add_mutate)\("(compile|mutate):([a-z0-9\-_?!+]+)"')

# Production-approved Guard entry (Issue #2124: try_acquire family only).
# Issue #2555: TransactionGuard host path is an approved entry (delegates to
# MutationBoundaryGuard::try_acquire / soft orch boundary).
TRY_ACQUIRE_PATTERNS = [
    re.compile(r"run_under_mutation_guard\("),
    re.compile(r"run_compile_dirty_under_guard\("),
    re.compile(r"with_compiler_service_pin\("),
    re.compile(r"MutationBoundaryGuard::try_acquire\("),
    re.compile(r"aura_orch_agent_body_try_acquire\("),
    re.compile(r"transaction_guard_host\("),
    re.compile(r"make_transaction_guard\("),
    re.compile(r"TransactionGuard\s+\w+\s*\("),
]

# Legacy ctor — residual; fails --strict under #2124 AC1.
LEGACY_CTOR_RE = re.compile(r"MutationBoundaryGuard\s+\w+\s*\(\s*(?:ev|evaluator)\s*,")

# Still count legacy as "has some Guard" for soft coverage % during
# transition reporting (AC2 wants try_acquire specifically).
SOFT_GUARD_PATTERNS = TRY_ACQUIRE_PATTERNS + [
    re.compile(r"MutationBoundaryGuard\s+\w+\s*\("),
]

READONLY_PATTERNS = [
    re.compile(r"\?\s*$"),
    re.compile(r"-stats\b"),
    re.compile(r"-count\b"),
    re.compile(r"query-"),
    re.compile(r"verify-dirty\?"),
    re.compile(r"is-"),
    re.compile(r"^set-agent-fingerprint$"),
    re.compile(r"^validate-reflected$"),
    re.compile(r"^validate-against-schema$"),
    re.compile(r"^snapshot$"),
    re.compile(r"^hw-bitvec-width$"),
    re.compile(r"^per-defuse-index-callers$"),
    # Issue #2124: metadata / policy / probe-only mutate:* (no AST write).
    re.compile(r"^check-stable-ref$"),
    re.compile(r"^set-stale-ref-policy$"),
    re.compile(r"^set-pattern-index-policy$"),
    re.compile(r"^request-gc-safepoint$"),
]

SCAN_DIR = REPO_ROOT / "src" / "compiler"
SCAN_GLOB = "evaluator_primitives_*.cpp"

# Production sources scanned for residual legacy ctor (#2124 AC1).
LEGACY_SCAN_GLOBS = (
    "evaluator_primitives_*.cpp",
    "verify_tool.cpp",
    "evaluator_fiber_mutation.cpp",
    "evaluator_mutation_boundary.cpp",
)


def is_readonly(name: str) -> bool:
    return any(p.search(name) for p in READONLY_PATTERNS)


def scan_file(path: Path) -> tuple[list[tuple[int, str, bool]], list[tuple[int, str, bool]]]:
    """Return (covered_try_acquire, uncovered) primitive entries."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return [], []
    covered: list[tuple[int, str, bool]] = []
    uncovered: list[tuple[int, str, bool]] = []
    lines = text.splitlines()
    for i, line in enumerate(lines, start=1):
        m = ADD_RE.search(line)
        if not m:
            continue
        kind, name = m.group(1), m.group(2)
        is_mutation_kind = kind == "mutate"
        if is_readonly(name):
            covered.append((i, f"{kind}:{name}", is_mutation_kind))
            continue
        # Issue #2124: long arg parsing (e.g. mutate:atomic-batch) may put
        # try_acquire ~100+ lines below add_mutate — window 160 lines.
        window_end = min(len(lines), i + 160)
        window = "\n".join(lines[i - 1 : window_end])
        # #2124: only try_acquire family counts as production covered.
        has_try = any(p.search(window) for p in TRY_ACQUIRE_PATTERNS)
        if has_try:
            covered.append((i, f"{kind}:{name}", is_mutation_kind))
        else:
            uncovered.append((i, f"{kind}:{name}", is_mutation_kind))
    return covered, uncovered


def find_legacy_ctors() -> list[tuple[str, int, str]]:
    """List residual production legacy ctor call sites (line content)."""
    out: list[tuple[str, int, str]] = []
    for glob in LEGACY_SCAN_GLOBS:
        for path in sorted(SCAN_DIR.glob(glob)):
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for i, line in enumerate(text.splitlines(), start=1):
                stripped = line.lstrip()
                if stripped.startswith("//") or stripped.startswith("*"):
                    continue
                # try_acquire lines also contain MutationBoundaryGuard::try_acquire —
                # exclude those.
                if "try_acquire" in line:
                    continue
                if "doomed" in line:  # move-assign release
                    continue
                if LEGACY_CTOR_RE.search(line):
                    rel = str(path.relative_to(REPO_ROOT))
                    out.append((rel, i, line.strip()))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument(
        "--strict",
        action="store_true",
        help="Exit 1 if uncovered primitives OR residual legacy ctors (#2124).",
    )
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

    legacy = find_legacy_ctors()
    total = total_covered + total_uncovered
    pct = (100.0 * total_covered / total) if total > 0 else 0.0

    if args.quiet:
        print(
            f"mutation_guard_coverage: {total_covered}/{total} covered ({pct:.1f}%) — "
            f"{total_uncovered} uncovered, {len(legacy)} legacy-ctor residual "
            f"(Issue #1950 / #2124)"
        )
        if args.strict and (total_uncovered > 0 or legacy):
            return 1
        return 0

    print("Issue #1950 / #2124 mutation_guard_coverage report:")
    print(f"  total primitives scanned : {total}")
    print(f"  covered (try_acquire+)   : {total_covered} ({pct:.1f}%)")
    print(f"  uncovered                : {total_uncovered}")
    print(f"  legacy ctor residual     : {len(legacy)}  (#2124 AC1 target 0)")
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
    if legacy:
        print()
        print("  Residual legacy MutationBoundaryGuard ctor (#2124 AC1):")
        for rel, lineno, line in legacy[:40]:
            print(f"    {rel}:{lineno}: {line[:100]}")
        if len(legacy) > 40:
            print(f"    ... and {len(legacy) - 40} more")

    fail = False
    if args.strict and total_uncovered > 0:
        print(
            f"\nFAIL: {total_uncovered} uncovered primitive(s) without try_acquire family.",
            file=sys.stderr,
        )
        fail = True
    if args.strict and legacy:
        print(
            f"\nFAIL: {len(legacy)} residual legacy MutationBoundaryGuard ctor site(s) "
            f"(Issue #2124 AC1 — migrate to try_acquire).",
            file=sys.stderr,
        )
        fail = True
    if fail:
        return 1
    if args.strict:
        print("\nOK: 100% try_acquire coverage + 0 legacy ctor residual (#2124)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
