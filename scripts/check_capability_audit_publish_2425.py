#!/usr/bin/env python3
"""Issue #2425: CapabilityRegistry audit_ring published slots (no torn reads).

Contract:
  AC1 PublishedAuditSlot with publish_seq + data; try_load double-check
  AC2 concurrent gate test; record_audit exclusive ring mutex
  AC3 audit_seq fetch_add release; publish_seq store release; load acquire
  AC4 try_load_latest_audit + gate wiring

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

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
            fails.append(f"{label}: unexpected {n!r}")

    hh = _read("src/core/capability_model.hh")
    test = _read("tests/core/test_capability_audit_publish_2425.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2425", "AC1", hh)
    must("struct PublishedAuditSlot", "AC1", hh)
    must("std::atomic<std::uint64_t> publish_seq{0}", "AC1", hh)
    must("PublishedAuditSlot audit_ring[kAuditRing]", "AC1", hh)
    must_not("EffectAuditEntry audit_ring[kAuditRing]", "AC1", hh)
    must("try_load_audit_seq", "AC1", hh)
    must("2425 AC1", "AC1", test)

    must("audit_ring_mtx_", "AC2", hh)
    must("std::unique_lock<std::shared_mutex> wlock(audit_ring_mtx_)", "AC2", hh)
    must("std::shared_lock<std::shared_mutex> rlock(audit_ring_mtx_)", "AC2", hh)
    must("2425 AC2", "AC2", test)
    must("concurrent record_audit", "AC2", test)

    must("audit_seq.fetch_add(1, std::memory_order_release)", "AC3", hh)
    must("publish_seq.store(seq + 1, std::memory_order_release)", "AC3", hh)
    must("publish_seq.load(std::memory_order_acquire)", "AC3", hh)
    must("audit_seq.load(std::memory_order_acquire)", "AC3", hh)
    must("2425 AC3", "AC3", test)

    must("try_load_latest_audit", "AC4", hh)
    must("2425 AC4", "AC4", test)
    must("check_capability_audit_publish_2425", "gate", build)
    must("cmd_capability_audit_publish_coverage", "gate", build)
    must("test_capability_audit_publish_2425", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: capability audit publish #2425 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
