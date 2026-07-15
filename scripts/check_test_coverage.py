#!/usr/bin/env python3
"""Issue #1453: test coverage / binding umbrella for CI.

Runs:
  1. scripts/check_test_binding.py  (hard pairing: prim sources ↔ tests/)
  2. scripts/gen_test_registry.py --check  (registry freshness, optional)

Usage:
  python3 scripts/check_test_coverage.py
  python3 scripts/check_test_coverage.py --base origin/main --require-name-mention

Exit 0 = OK, 1 = failure.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base", default="origin/main")
    ap.add_argument("--require-name-mention", action="store_true")
    ap.add_argument("--soft", action="store_true")
    ap.add_argument(
        "--skip-registry",
        action="store_true",
        help="skip gen_test_registry --check",
    )
    args = ap.parse_args()

    binding = ROOT / "scripts" / "check_test_binding.py"
    cmd = [sys.executable, str(binding), "--base", args.base]
    if args.require_name_mention:
        cmd.append("--require-name-mention")
    if args.soft:
        cmd.append("--soft")
    r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode != 0:
        return r.returncode

    if not args.skip_registry:
        gen = ROOT / "scripts" / "gen_test_registry.py"
        if gen.exists():
            r2 = subprocess.run(
                [sys.executable, str(gen), "--check"],
                cwd=ROOT,
            )
            if r2.returncode != 0:
                return r2.returncode

    print("OK: check_test_coverage (binding + registry)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
