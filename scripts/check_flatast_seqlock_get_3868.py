#!/usr/bin/env python3
"""Issue #3868: FlatAST::get seqlock (torn-NodeView machine-check) contract.

Contract:
  AC1 ast.ixx documents the #3868 seqlock: soa_write_epoch_ + RAII writer
      section inside add_node/clear + observability counter
  AC2 get() is the seqlock reader: bounded retry over assemble_nodeview,
      odd-epoch rejection, get_soa_safe() locked fallback; get_soa_safe
      stays SoAReadGuard-protected
  AC3 hot path stays lock-free: the get() seqlock body takes no mutex
  AC4 test binding: test_get_nodeview_snapshot hosts #3868 ACs (batch
      member via CMakeLists.txt)
  AC5 gate wiring: linter registered in build.py + coverage allowlist

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


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

    ast = _read("src/core/ast.ixx")
    test = _read("tests/core/test_get_nodeview_snapshot.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: seqlock epoch + writer sections ──────────────────────────
    must("Issue #3868", "AC1", ast)
    must("soa_write_epoch_", "AC1", ast)
    must("soa_get_torn_retry_total_", "AC1", ast)
    must("class SoaSeqlockWriteSection", "AC1", ast)
    if ast.count("SoaSeqlockWriteSection soa_epoch_section_") < 2:
        fails.append("AC1: writer section must open in BOTH add_node and clear")
    must("soa_write_epoch_.fetch_add(1, std::memory_order_release)", "AC1", ast)

    # ── AC2: get() seqlock reader + locked fallback ───────────────────
    must("SEQLOCK READER", "AC2", ast)
    must("NodeView assemble_nodeview(NodeId id) const", "AC2", ast)
    must("(e0 & 1u) != 0u", "AC2", ast)
    must("return get_soa_safe(id);", "AC2", ast)
    must("SoAReadGuard guard(this);", "AC2", ast)

    # ── AC3: hot path lock-free (no mutex in the seqlock body) ────────
    start = ast.find("NodeView get(NodeId id) const {")
    end = ast.find("NodeView assemble_nodeview(NodeId id) const")
    if start == -1 or end == -1 or end <= start:
        fails.append("AC3: could not locate get() seqlock body span")
    else:
        body = ast[start:end]
        for tok in ("unique_lock", "shared_lock", "lock_guard", "SoAReadGuard"):
            if tok in body:
                fails.append(f"AC3: hot-path get() must not take {tok}")
        if "return v;" not in body or "attempt" not in body:
            fails.append("AC3: seqlock retry loop missing from get()")

    # ── AC4: test binding (batch member) ──────────────────────────────
    for ac in ("3868 AC4", "3868 AC5", "3868 AC6", "3868 AC7"):
        must(ac, "AC4", test)
    must("torn-retry detector", "AC4", test)
    must("begin_soa_write", "AC4", test)
    must("run_test_get_nodeview_snapshot", "AC4", test)
    must("tests/core/test_get_nodeview_snapshot.cpp", "AC4", cmake)

    # ── AC5: gate wiring ──────────────────────────────────────────────
    must("check_flatast_seqlock_get_3868", "AC5", build)
    must("check_flatast_seqlock_get_3868.py", "AC5", allow)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: FlatAST seqlock get #3868 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
