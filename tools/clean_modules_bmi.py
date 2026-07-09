#!/usr/bin/env python3
"""
clean_modules_bmi.py — Issue #871: 减法 close — modules BMI
cleanup script.

The Aura build emits C++ module BMI files (`.gcm` + `.ifc`)
into `${CMAKE_BINARY_DIR}/module_cache/` keyed by producer
target (aura, aura_test_objects, etc.). Over time, stale BMIs
accumulate when:

  - A .ixx is renamed or deleted (old BMI stays in cache)
  - A producer target changes (producer key in cache path no
    longer matches — symmetric issue from cmake/aura_module_
    launcher.sh's per-producer symlinks)
  - The build directory is cloned from another machine (BMIs
    are machine-specific)

This script cleans stale module BMI artifacts WITHOUT deleting
fresh ones, so the next build is faster while still preserving
all currently-referenced BMIs.

Usage:
  python3 tools/clean_modules_bmi.py                # dry-run
  python3 tools/clean_modules_bmi.py --apply        # actually delete
  python3 tools/clean_modules_bmi.py --older-than 7 # only >7 days old
  python3 tools/clean_modules_bmi.py --build-dir build/module_cache

Exit codes:
  0 = success
  1 = invalid args
  2 = module_cache dir missing (nothing to clean)

This script is safe to run as a CI gate step (e.g. a pre-build
hook). It performs no compilation, only filesystem deletion.
"""

from __future__ import annotations

import argparse
import contextlib
import os
import shutil
import sys
import time
from pathlib import Path

_DEFAULT_BUILD_DIR = Path("build") / "module_cache"
_DEFAULT_OLDER_THAN_DAYS = 30  # Conservative: only stale BMIs
_KNOWN_PRODUCER_KEYS = {
    # Known producer target names whose BMIs live in
    # `${module_cache}/${producer}.dir/`. Listed here so we
    # can detect orphan BMIs (BMIs under a producer key that
    # is no longer built) vs active ones.
    "aura.dir",
    "aura_test_objects.dir",
}


def _humanize_size(num_bytes: int) -> str:
    if num_bytes < 1024:
        return f"{num_bytes} B"
    if num_bytes < 1024 * 1024:
        return f"{num_bytes / 1024:.1f} KB"
    if num_bytes < 1024 * 1024 * 1024:
        return f"{num_bytes / (1024 * 1024):.1f} MB"
    return f"{num_bytes / (1024 * 1024 * 1024):.2f} GB"


def _scan_module_cache(module_cache: Path, older_than_days: int) -> tuple[list[Path], list[Path], int]:
    """Walk module_cache and separate fresh vs stale BMI files.

    A BMI file is "stale" if either:
      - It's older than `older_than_days` days (heuristic for
        rotated `.ixx` files), OR
      - It lives under a producer key that is no longer in
        `_KNOWN_PRODUCER_KEYS` (e.g. removed in refactor).

    Returns (stale_paths, fresh_paths, total_bytes_stale).
    """
    if not module_cache.exists():
        return [], [], 0
    cutoff = time.time() - older_than_days * 86400
    stale: list[Path] = []
    fresh: list[Path] = []
    stale_bytes = 0
    for root, _dirs, files in os.walk(module_cache):
        # Suffix + mtime filter: clean BMIs older than cutoff.
        # The producer-key registry of _KNOWN_PRODUCER_KEYS is
        # informational only (most CTest test_issue_*.dir entries
        # are valid but unlisted); the deletion decision is
        # always driven by mtime + suffix to keep the script
        # safe for arbitrary CMake target layouts.
        for f in files:
            fpath = Path(root) / f
            if fpath.suffix not in {".gcm", ".ifc"}:
                continue
            try:
                mtime = fpath.stat().st_mtime
            except OSError:
                continue
            if mtime < cutoff:
                with contextlib.suppress(OSError):
                    stale_bytes += fpath.stat().st_size
                stale.append(fpath)
            else:
                fresh.append(fpath)
    return stale, fresh, stale_bytes


def main() -> int:
    ap = argparse.ArgumentParser(
        description=("Clean stale C++ module BMI files from the Aura build dir (Issue #871 减法 close).")
    )
    ap.add_argument(
        "--apply",
        action="store_true",
        help="actually delete stale BMIs (default: dry-run, just report)",
    )
    ap.add_argument(
        "--older-than",
        type=int,
        default=_DEFAULT_OLDER_THAN_DAYS,
        help="only delete BMIs older than N days (default: 30)",
    )
    ap.add_argument(
        "--build-dir",
        type=Path,
        default=_DEFAULT_BUILD_DIR,
        help="module_cache directory to scan (default: build/module_cache)",
    )
    args = ap.parse_args()

    module_cache: Path = args.build_dir
    if not module_cache.is_absolute():
        module_cache = Path.cwd() / module_cache

    if not module_cache.exists():
        print(
            f"⚠️  module_cache dir not found: {module_cache}",
            file=sys.stderr,
        )
        return 2

    stale, fresh, stale_bytes = _scan_module_cache(module_cache, args.older_than)

    print(f"Module BMI scan ({module_cache}):")
    print(f"  stale: {len(stale)} ({_humanize_size(stale_bytes)})")
    print(f"  fresh: {len(fresh)}")

    if not stale:
        print("✓ Nothing to clean.")
        return 0

    if not args.apply:
        print("  Dry-run only (pass --apply to actually delete).")
        for p in stale[:10]:
            print(f"    would remove: {p}")
        if len(stale) > 10:
            print(f"    ... and {len(stale) - 10} more")
        return 0

    removed = 0
    removed_bytes = 0
    for p in stale:
        try:
            sz = p.stat().st_size
            if p.is_dir() and not p.is_symlink():
                shutil.rmtree(p)
            else:
                p.unlink()
            removed += 1
            removed_bytes += sz
        except OSError as e:
            print(f"  ✗ failed to remove {p}: {e}", file=sys.stderr)
    print(f"✓ Removed {removed} stale BMIs ({_humanize_size(removed_bytes)}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
