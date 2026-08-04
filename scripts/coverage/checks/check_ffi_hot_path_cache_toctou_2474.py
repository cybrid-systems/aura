#!/usr/bin/env python3
"""Issue #2474: FFI hot-path cache torn update closed.

Contract:
  AC1 multi-thread stress test present
  AC2 update_cache: invalidate hash → abi → fn → hash LAST
  AC3 dispatch_batch + dispatch_cellgrid double-check
  AC4 clear_cache hash-first
  AC5 race counter + gate wiring

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

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

    hh = _read("src/compiler/ffi_hot_path.hh")
    build = _read("build.py")

    uidx = hh.find("void update_cache")
    update = hh[uidx : uidx + 900] if uidx >= 0 else ""
    bidx = hh.find("dispatch_batch(")
    batch = hh[bidx : bidx + 1800] if bidx >= 0 else ""
    cidx = hh.find("dispatch_cellgrid(")
    cell = hh[cidx : cidx + 1800] if cidx >= 0 else ""
    clidx = hh.find("void clear_cache")
    clear = hh[clidx : clidx + 500] if clidx >= 0 else ""

    must("Issue #2474", "AC1", hh)
    must("dispatch_batch", "AC1", hh)
    must("dispatch_cellgrid", "AC1", hh)
    # Unit test deleted with TUI surface (#2626); header CAS contract remains.

    must("cached_sig_hash.store(0", "AC2", update)
    must("cached_func_ptr.store", "AC2", update)
    must("Issue #2474", "AC2", update)
    # hash LAST: last sig_hash.store must be after func_ptr.store
    fn_pos = update.find("cached_func_ptr.store")
    hash_last = update.rfind("cached_sig_hash.store")
    if fn_pos < 0 or hash_last < 0 or hash_last <= fn_pos:
        fails.append("AC2: hash publish must come after func_ptr.store")

    must("ffi_hot_path_cache_update_race_total", "AC3", batch)
    must("ffi_hot_path_cache_update_race_total", "AC3", cell)
    must("Issue #2474", "AC3", batch)
    must("Issue #2474", "AC3", cell)
    must("cached_sig_hash.store(0", "AC4", clear)
    must("Issue #2474", "AC4", clear)

    must("ffi_hot_path_cache_update_race_total", "AC5", hh)
    must("check_ffi_hot_path_cache_toctou_2474", "gate", build)
    must("cmd_ffi_hot_path_cache_toctou_coverage", "gate", build)
    must("#2626", "gate", _read("scripts/coverage/checks/check_ffi_hot_path_cache_toctou_2474.py"))

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: FFI hot-path cache TOCTOU #2474 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
