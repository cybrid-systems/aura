#!/usr/bin/env python3
"""Issue #2374: remove dead legacy pin_registry densify walk + API.

Contract:
  AC1 densify selective-invalidate uses sharded helper (not pin_registry())
  AC2 pin_registry() / pin_registry_mtx() removed from lifetime_pin SSOT (.hh)
  AC3 invalidate_pins_not_in_new_addrs preserves non-remapped pin nulling
  AC4 unit test / gate wire cites #2374

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


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden residue {n!r}")

    arena = _read("src/core/arena.ixx")
    pin = _read("src/core/lifetime_pin.hh")  # SSOT
    pin_mod = _read("src/core/lifetime_pin.ixx")
    test = _read("tests/core/test_moving_compact.cpp")
    bp = _read("build.py")

    # AC1: densify no longer walks legacy registry
    must("invalidate_pins_not_in_new_addrs", "AC1", arena)
    must("Issue #2374", "AC1", arena)
    must_not("pin_registry_mtx()", "AC1", arena)
    # Bare pin_registry() call site (not in comments about removal)
    if re.search(r"lifetime::pin_registry\(\)", arena):
        fails.append("AC1: arena still calls lifetime::pin_registry()")

    # AC2: dead API removed from module
    must_not("inline std::vector<LifetimePin*>& pin_registry()", "AC2", pin)
    must_not("inline std::mutex& pin_registry_mtx()", "AC2", pin)
    must("pin_registry_shards", "AC2", pin)
    must("Issue #2374", "AC2", pin)
    must_not("inline std::vector<LifetimePin*>& pin_registry()", "AC2", pin_mod)
    must_not("inline std::mutex& pin_registry_mtx()", "AC2", pin_mod)

    # AC3: sharded selective invalidate helper
    must("invalidate_pins_not_in_new_addrs", "AC3", pin)
    must("unpin_on_compact", "AC3", pin)
    must("kPinRegistryShardCount", "AC3", pin)
    # AC_M5: live pins block Moving densify (AC_M3 contract); non-arena
    # pin case remains. Selective invalidate is exercised via #2374 helper.
    must("AC_M5: Moving blocked with live non-arena pin", "AC3", test)
    must("invalidate_pins_not_in_new_addrs", "AC3", test)
    must("Issue #2374", "AC3", test)

    # AC4: gate + source cites
    must("cmd_legacy_pin_registry_cleanup_coverage", "AC4", bp)
    must("check_legacy_pin_registry_cleanup_2374.py", "AC4", bp)
    must("Issue #2374", "AC4", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2374 legacy pin_registry cleanup — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
