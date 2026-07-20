#!/usr/bin/env python3
"""tests/migrate_test_layout.py — Issue #1932 / #1937 layout migration.

Idempotent migration helper for grouping Python drivers under
tests/python/, bench drivers under tests/bench/, and reserving
tests/fuzz/ + tests/memory/ for future campaigns.

Usage:
  python3 tests/migrate_test_layout.py --dry-run   # print plan only
  python3 tests/migrate_test_layout.py --apply     # git mv remaining files

The primary move already landed with #1932. This script documents the
target layout and re-applies any leftover top-level drivers.
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

DIRS = ("python", "bench", "fuzz", "memory")


def plan() -> list[tuple[Path, Path]]:
    out: list[tuple[Path, Path]] = []
    for src_rel, dst_rel in MOVES:
        src = TESTS / src_rel
        dst = TESTS / dst_rel
        # Only move if source is a real file (not thin entrypoint) and dest missing.
        if not src.is_file():
            continue
        # Skip thin entrypoints (small runpy wrappers left at root).
        text = src.read_text(encoding="utf-8", errors="replace")
        if "Thin entrypoint" in text or "Issue #1932 layout" in text:
            continue
        if dst.exists():
            continue
        out.append((src, dst))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--dry-run", action="store_true")
    g.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    for d in DIRS:
        (TESTS / d).mkdir(parents=True, exist_ok=True)

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


if __name__ == "__main__":
    sys.exit(main())
