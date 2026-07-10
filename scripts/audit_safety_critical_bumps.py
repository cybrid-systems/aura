#!/usr/bin/env python3
"""Issue #1149: ensure safety-critical bump_* methods have call sites."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "compiler"

# Phase 1 allowlist of safety-critical helpers that must not be dead.
CRITICAL = [
    "bump_panic_checkpoint_save_count",
    "bump_panic_checkpoint_restore_count",
    "bump_panic_checkpoint_commit_count",
    "bump_panic_checkpoint_transfer_count",
    "bump_concurrent_safety_recovery_success",
]


def call_count(name: str) -> int:
    # Include evaluator.ixx (public methods often wrap bumps inline).
    r = subprocess.run(
        ["rg", "-n", rf"\b{re.escape(name)}\s*\(", str(SRC)],
        capture_output=True,
        text=True,
        check=False,
    )
    # Subtract pure declarations (void name(...))
    n = 0
    for ln in r.stdout.splitlines():
        if "void " + name in ln:
            continue
        if name in ln:
            n += 1
    return n


def main() -> int:
    dead = []
    for m in CRITICAL:
        n = call_count(m)
        print(f"{m}={n}")
        if n == 0:
            dead.append(m)
    if dead:
        print("DEAD safety-critical bumps:", ", ".join(dead), file=sys.stderr)
        return 1
    print("safety-critical bumps: all wired")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
