#!/usr/bin/env python3
"""Issue #2549: is_stealable trusts MutationSafetySnapshot only.

Contract:
  AC1 is_steal_candidate (reason class) + is_stealable = candidate && safe
  AC2 production try_steal_from uses is_stealable(snap); defer uses candidate
  AC3 fiber_steal_priority uses is_steal_candidate (not bare is_stealable)
  AC4 no residual bare is_stealable() enqueue dual-check in production serve/
  AC5 test + cmake + build.py gate wired; fiber.h documents contract

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: residual forbidden pattern {n!r}")

    fh = _read("src/serve/fiber.h")
    wc = _read("src/serve/worker.cpp")
    wh = _read("src/serve/worker.h")
    test = _read("tests/serve/test_is_stealable_snapshot_gate_2549.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1: API split
    must("Issue #2549", "AC1", fh)
    must("is_steal_candidate()", "AC1", fh)
    must("is_stealable() const noexcept", "AC1", fh)
    must("is_steal_candidate() && is_at_mutation_boundary_safe()", "AC1", fh)
    must("Steal safety is defined solely by MutationSafetySnapshot", "AC1", fh)
    must("is_stealable(const MutationSafetySnapshot& s)", "AC1", fh)

    # AC2: production steal entries
    must("is_stealable(snap)", "AC2", wc)
    must("is_steal_candidate(snap)", "AC2", wc)
    must("Issue #2549", "AC2", wc)
    must_not("stolen->is_stealable() &&", "AC2", wc)
    must_not("is_stealable() && stolen->is_at_mutation_boundary_safe", "AC2", wc)

    # AC3: priority scoring
    must("is_steal_candidate()", "AC3", wh)
    must("Issue #2549", "AC3", wh)
    must_not("!fiber->is_stealable()", "AC3", wh)

    # AC4: residual bare is_stealable() in production serve decision points
    # Allow definitions in fiber.h and comments; forbid production call forms
    # that use reason-class-only semantics (bare is_stealable() as enqueue gate).
    for rel in (
        "src/serve/worker.cpp",
        "src/serve/worker.h",
        "src/serve/scheduler.cpp",
        "src/serve/scheduler.h",
        "src/serve/multi_fiber_mailbox.h",
    ):
        src = _read(rel)
        if not src:
            continue
        # Bare call is_stealable() without snap arg in non-comment lines
        for i, line in enumerate(src.splitlines(), 1):
            stripped = line.lstrip()
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                continue
            # Definition of is_stealable itself only lives in fiber.h
            if re.search(r"\bis_stealable\s*\(\s*\)", line) and "is_stealable(snap)" not in line:
                # allow method definitions only in fiber.h (not here)
                fails.append(f"AC4: {rel}:{i}: bare is_stealable() call (use snap or candidate)")

    # AC5: wiring
    must("ac1_held_or_unsafe_mb_not_stealable", "AC5", test)
    must("ac3_production_call_sites", "AC5", test)
    must("test_is_stealable_snapshot_gate_2549", "AC5", cmake)
    must("check_is_stealable_snapshot_gate_2549", "AC5", build)
    must("cmd_is_stealable_snapshot_gate_coverage", "AC5", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2549 is_stealable snapshot gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
