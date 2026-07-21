#!/usr/bin/env python3
"""tests/migrate_test_layout.py — Issue #1932 / #1937 / #1939 layout migration.

Idempotent migration helper for grouping Python drivers under
tests/python/, bench drivers under tests/bench/, and reserving
tests/fuzz/. Final cleanup / inventory: #1939.

Usage:
  python3 tests/migrate_test_layout.py --dry-run   # print pending moves only
  python3 tests/migrate_test_layout.py --apply     # git mv remaining files
  python3 tests/migrate_test_layout.py --status    # inventory + policy check

The primary move landed with #1932. This script documents the target layout,
re-applies any leftover top-level drivers, and reports root policy health.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests"

# (src relative to tests/, dest relative to tests/)
MOVES: list[tuple[str, str]] = [
    # Harness / runners → python/
    ("_aura_harness.py", "python/_aura_harness.py"),
    ("issue_tier.py", "python/issue_tier.py"),
    ("run.py", "python/run.py"),
    ("run_issue_tests.py", "python/run_issue_tests.py"),
    ("integ_cases.py", "python/integ_cases.py"),
    ("smoke_cases.py", "python/smoke_cases.py"),
    ("regression_cases.py", "python/regression_cases.py"),
    ("fixture_check.py", "python/fixture_check.py"),
    ("fixture_store.py", "python/fixture_store.py"),
    ("check_gradual.py", "python/check_gradual.py"),
    ("repl_test.py", "python/repl_test.py"),
    ("mutation_loop.py", "python/mutation_loop.py"),
    ("test_audit_catch_silent_swallow.py", "python/test_audit_catch_silent_swallow.py"),
    ("test_audit_dead_heap_push.py", "python/test_audit_dead_heap_push.py"),
    ("test_primitive_surface_gate.py", "python/test_primitive_surface_gate.py"),
    ("test_test_binding_gate.py", "python/test_test_binding_gate.py"),
    ("test_regression.py", "python/test_regression.py"),
    ("run-tests.sh", "python/run-tests.sh"),
    ("run_sanitizer_matrix.sh", "python/run_sanitizer_matrix.sh"),
    ("run_concurrent_edsl_tsan.sh", "python/run_concurrent_edsl_tsan.sh"),
    ("run_issue_218_tsan.sh", "python/run_issue_218_tsan.sh"),
    # Bench Python drivers → bench/
    ("benchmark.py", "bench/benchmark.py"),
    ("benchmark_cases.py", "bench/benchmark_cases.py"),
    ("benchmark_baseline.json", "bench/benchmark_baseline.json"),
    ("run_bench_all.py", "bench/run_bench_all.py"),
]

# Required category dirs after #1932 / #1934 / #1935 / #1939 / #1977.
# Domain stays the "preferred suites" home; theme subdirs at tests/ root
# hold bulk legacy root cpp (#1977). arena is BOTH a domain pilot
# (tests/domain/arena/, 5 files) and a root theme home (tests/arena/, 7 files).
DIRS = (
    "python",
    "bench",
    "fuzz",
    "e2e",
    "domain",
    "suite",
    "fixtures",
    "observability",
    "mutation",
    "compiler_core",
    "fiber",
    "edsl",
    "jit",
    "arena",
    "stdlib",
    "linear",
    "shape",
    "misc",
)

# Stable CLI shims intentionally left at tests/ root (#1932 / #1939).
ALLOWED_THIN_ENTRYPOINTS = frozenset(
    {
        "run.py",
        "run_issue_tests.py",
        "fixture_check.py",
        "check_gradual.py",
        "mutation_loop.py",
        "repl_test.py",
        "benchmark.py",
        "run_bench_all.py",
        "run-tests.sh",
    }
)

# Non-driver files that may remain at tests/ root (headers, policy, docs).
ALLOWED_ROOT_OTHER = frozenset(
    {
        "README.md",
        "STRATEGY.md",
        "migrate_test_layout.py",
        "legacy_test_inventory.md",
        "domain_classification.md",
        "root_test_classification.md",  # scripts/classify_root_tests.py
        "test-binding-allowlist.txt",
        "test_harness.hpp",
        "nodeview_wire.hh",
        # test_issue_178_bridge.h lives under tests/issues/ (next to the two TUs)
        "runtime_test_harness.c",
    }
)


def is_thin_entrypoint(path: Path) -> bool:
    if not path.is_file():
        return False
    text = path.read_text(encoding="utf-8", errors="replace")
    return "Thin entrypoint" in text or "Issue #1932 layout" in text


def plan() -> list[tuple[Path, Path]]:
    out: list[tuple[Path, Path]] = []
    for src_rel, dst_rel in MOVES:
        src = TESTS / src_rel
        dst = TESTS / dst_rel
        # Only move if source is a real file (not thin entrypoint) and dest missing.
        if not src.is_file():
            continue
        if is_thin_entrypoint(src):
            continue
        if dst.exists():
            continue
        out.append((src, dst))
    return out


def root_script_inventory() -> tuple[list[str], list[str], list[str]]:
    """Return (ok_thin, unexpected_full_drivers, unexpected_other)."""
    ok_thin: list[str] = []
    bad_full: list[str] = []
    unexpected: list[str] = []
    for p in sorted(TESTS.iterdir()):
        if not p.is_file():
            continue
        name = p.name
        if name.startswith("."):
            continue
        if name.endswith(".cpp"):
            continue  # C++ bulk — migrate via #1957 domain waves, not this script
        if name.endswith((".py", ".sh")):
            if name == "migrate_test_layout.py":
                continue
            if name in ALLOWED_THIN_ENTRYPOINTS and is_thin_entrypoint(p):
                ok_thin.append(name)
            elif name in ALLOWED_THIN_ENTRYPOINTS:
                bad_full.append(f"{name} (expected thin entrypoint)")
            else:
                bad_full.append(name)
            continue
        if name not in ALLOWED_ROOT_OTHER:
            unexpected.append(name)
    return ok_thin, bad_full, unexpected


def status() -> int:
    print(f"Layout status under {TESTS}  (#1932 / #1937 / #1939)\n")
    print("Required dirs:")
    missing = []
    for d in DIRS:
        present = (TESTS / d).is_dir()
        mark = "OK" if present else "MISSING"
        print(f"  [{mark}] tests/{d}/")
        if not present:
            missing.append(d)

    moves = plan()
    print(f"\nPending driver moves: {len(moves)}")
    for src, dst in moves:
        print(f"  {src.relative_to(ROOT)} → {dst.relative_to(ROOT)}")

    ok_thin, bad_full, unexpected = root_script_inventory()
    print(f"\nThin entrypoints at root ({len(ok_thin)}):")
    for name in ok_thin:
        print(f"  OK  {name}")

    print(f"\nUnexpected full drivers / non-thin scripts ({len(bad_full)}):")
    if not bad_full:
        print("  (none)")
    for name in bad_full:
        print(f"  BAD {name}")

    print(f"\nUnexpected non-cpp root files ({len(unexpected)}):")
    if not unexpected:
        print("  (none)")
    for name in unexpected:
        print(f"  ?   {name}")

    # Count policy: non-cpp tracked-ish files at root (excluding __pycache__)
    non_cpp = [
        p.name for p in TESTS.iterdir() if p.is_file() and not p.name.endswith(".cpp") and not p.name.startswith(".")
    ]
    print(f"\nNon-C++ top-level files: {len(non_cpp)}")
    print("  (C++ test_*.cpp bulk stays at root until domain migration #1957)")
    print("  Policy: only thin entrypoints + harness headers + allowlist + docs.")

    rc = 0
    if missing:
        print(f"\nFAIL: missing dirs: {', '.join(missing)}")
        rc = 1
    if moves:
        print(f"\nFAIL: {len(moves)} pending move(s) — run --apply")
        rc = 1
    if bad_full:
        print(f"\nFAIL: {len(bad_full)} unexpected driver(s) at tests/ root")
        rc = 1
    if unexpected:
        print(f"\nWARN: {len(unexpected)} unexpected non-driver root file(s)")
        # warn only — allowlist may grow intentionally
    if rc == 0:
        print("\nOK: layout policy clean (#1939).")
    return rc


def apply_moves(moves: list[tuple[Path, Path]]) -> int:
    for src, dst in moves:
        dst.parent.mkdir(parents=True, exist_ok=True)
        r = subprocess.run(
            ["git", "mv", str(src), str(dst)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            # Fallback if not tracked yet
            shutil.move(str(src), str(dst))
            print(f"  moved (filesystem): {src.name}")
        else:
            print(f"  git mv: {src.name}")
    print("Done. Keep thin entrypoints at tests/ root for CLI stability.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--dry-run", action="store_true", help="print pending moves only")
    g.add_argument("--apply", action="store_true", help="git mv remaining full drivers")
    g.add_argument(
        "--status",
        action="store_true",
        help="inventory + root policy check (exit 1 if unclean) (#1939)",
    )
    args = ap.parse_args()

    for d in DIRS:
        (TESTS / d).mkdir(parents=True, exist_ok=True)

    if args.status:
        return status()

    moves = plan()
    print(f"Target layout under {TESTS}:")
    for d in DIRS:
        print(f"  tests/{d}/")
    print(f"\nPending moves: {len(moves)}")
    for src, dst in moves:
        print(f"  {src.relative_to(ROOT)} → {dst.relative_to(ROOT)}")

    if args.dry_run or not moves:
        if not moves:
            print("Nothing to do (layout already migrated).")
        return 0

    return apply_moves(moves)


if __name__ == "__main__":
    sys.exit(main())
