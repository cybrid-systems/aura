#!/usr/bin/env python3
"""Issue #2375: (restamp|invalidate)_all_pins_for_arena(0) all-shard walk.

#2342 regression: arena_id==0 was documented as "matches all" but only
walked pin_registry_shards()[0]. Boundary restamp + GC invalidate missed
shards[1..15].

Contract:
  AC1 arena_id==0 walks all kPinRegistryShardCount shards
  AC2 arena_id!=0 still single-shard
  AC3 lock order 0..15 (for loop over i)
  AC4 unit test ac2375 in test_moving_compact
  AC5 production callers still named (restamp/invalidate_all)
  AC6 Issue #2375 cite + gate wire

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _fn_body(src: str, name: str) -> str:
    """Rough extract of an inline function body by name."""
    m = re.search(
        rf"inline std::size_t\s+{re.escape(name)}\s*\([^)]*\)\s*noexcept\s*\{{",
        src,
    )
    if not m:
        return ""
    start = m.end() - 1  # at '{'
    depth = 0
    for i in range(start, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start : i + 1]
    return ""


def main() -> int:
    fails: list[str] = []

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    pin = _read("src/core/lifetime_pin.ixx")
    test = _read("tests/core/test_moving_compact.cpp")
    bp = _read("build.py")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    gc = _read("src/compiler/evaluator_gc.cpp")

    must("Issue #2375" in pin, "AC6: lifetime_pin.ixx cites #2375")
    must("restamp_all_pins_for_arena" in pin, "AC1: restamp_all present")
    must("invalidate_all_pins_for_arena" in pin, "AC1: invalidate_all present")

    for name in ("restamp_all_pins_for_arena", "invalidate_all_pins_for_arena"):
        body = _fn_body(pin, name)
        must(body != "", f"AC1: could not extract body of {name}")
        must("if (arena_id == 0)" in body, f"AC1: {name} has arena_id==0 branch")
        must(
            "for (std::size_t i = 0; i < kPinRegistryShardCount; ++i)" in body,
            f"AC1/AC3: {name} walks all shards in index order when arena_id==0",
        )
        # Single-shard path still present for N!=0
        must(
            "pin_registry_shard_index(arena_id)" in body,
            f"AC2: {name} still uses single-shard index for N!=0",
        )

    # Production callers (arg may be `0` or `std::uint64_t{0}`).
    must(
        re.search(
            r"restamp_all_pins_for_arena\s*\(\s*(?:std::uint64_t\s*\{\s*0\s*\}|0)\s*,",
            mb,
        )
        is not None,
        "AC5: boundary dtor restamp_all arena_id==0",
    )
    must("invalidate_all_pins_for_arena(0)" in gc, "AC5: GC invalidate_all(0)")

    # Unit test
    must("ac2375_all_shards_on_arena_zero" in test, "AC4: ac2375 test present")
    must("AC1: restamp_all(0) visits" in test or "AC1: restamp_all(0)" in test, "AC4: AC1 restamp")
    must("AC1: invalidate_all(0)" in test, "AC4: AC1 invalidate")
    must("AC2: restamp_all(3)" in test or "AC2: arena-3 pin" in test, "AC4: AC2 single-shard")
    must("Issue #2375" in test, "AC4: test cites #2375")

    # Gate
    must("cmd_pin_bulk_all_shards_coverage" in bp, "AC6: gate cmd wired")
    must("check_pin_bulk_all_shards_2375.py" in bp, "AC6: linter wired")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2375 bulk pin all-shard walk — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
