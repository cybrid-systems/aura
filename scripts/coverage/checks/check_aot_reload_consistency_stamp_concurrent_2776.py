#!/usr/bin/env python3
"""Issue #2776: AotReloadConsistencyProof concurrent stamp (no lost-update RMW).

stamp_aot_reload_consistency_proof used load+1 then store for stamp_epoch
(lost-update under concurrent writers) and 9 sequential stores without a
seqlock (torn multi-field snapshots for Agents).

Contract (one row per AC):
  AC1 stamp_epoch via fetch_add; no load+1 then store pattern on stamp_epoch
  AC2 seqlock g_aot_reload_proof_seq (even=stable, odd=writing)
  AC3 load_aot_reload_consistency_proof_snapshot / build_from_live use seqlock
  AC4 ac2776_* concurrent tests + this linter wired; no docs/design/*

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

    thin = _read("src/compiler/aot_reload_consistency_proof.h")
    bridge = _read("src/compiler/aura_jit_bridge.cpp")
    test = _read("tests/compiler/test_reload_recovery_query.cpp")
    build = _read("build.py")

    # AC1 — fetch_add stamp_epoch; ban lost-update RMW
    must("#2776", "AC1", thin)
    must("kAotReloadConsistencyStampConcurrentIssue", "AC1", thin)
    must("g_aot_reload_proof_stamp_epoch.fetch_add", "AC1", thin)
    # Detect classic lost-update: stamp_epoch = ...load... + 1 then later store stamp_epoch
    # Allow build_from_live local prep (p.stamp_epoch = p.stamp_epoch + 1) but ban
    # g_aot_reload_proof_stamp_epoch.load(...) + 1 used as the publish path.
    if re.search(
        r"g_aot_reload_proof_stamp_epoch\.load\s*\([^)]*\)\s*\+\s*1",
        thin,
    ):
        fails.append("AC1: lost-update RMW pattern g_aot_reload_proof_stamp_epoch.load()+1 still present")
    # stamp() must not store p.stamp_epoch into the global
    m = re.search(
        r"void\s+stamp_aot_reload_consistency_proof\s*\([^)]*\)\s*noexcept\s*\{(.*?)\n\}",
        thin,
        re.DOTALL,
    )
    if not m:
        fails.append("AC1: could not extract stamp_aot_reload_consistency_proof body")
    else:
        body = m.group(1)
        if "fetch_add" not in body:
            fails.append("AC1: stamp body missing fetch_add")
        if re.search(r"g_aot_reload_proof_stamp_epoch\.store\s*\(\s*p\.stamp_epoch", body):
            fails.append("AC1: stamp still stores p.stamp_epoch (lost-update path)")

    # AC2 — multi-writer seqlock (CAS even→odd claim)
    must("g_aot_reload_proof_seq", "AC2", thin)
    must("compare_exchange_weak", "AC2", thin)
    must("fetch_add(1", "AC2", thin)
    # odd writer / even stable
    if "& 1" not in thin and "& 1u" not in thin:
        fails.append("AC2: seqlock odd-phase check missing")
    must("atomic_thread_fence", "AC2", thin)

    # AC3 — snapshot reader
    must("load_aot_reload_consistency_proof_snapshot", "AC3", thin)
    must("build_aot_reload_consistency_proof_from_live", "AC3", thin)
    # build uses snapshot
    must("load_aot_reload_consistency_proof_snapshot()", "AC3", thin)

    # AC4 — tests + wire; bridge cites #2776
    must("ac2776_1_fetch_add_and_seqlock_source", "AC4", test)
    must("ac2776_2_concurrent_stamp_monotonic", "AC4", test)
    must("ac2776_3_reader_no_tear", "AC4", test)
    must("ac2776_4_source_cite_linter", "AC4", test)
    must("check_aot_reload_consistency_stamp_concurrent_2776", "AC4", build)
    must("#2776", "AC4", bridge)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2776-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2776.cpp").is_file():
        fails.append("AC4: test_issue_2776.cpp present (forbidden per #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2776 AotReloadConsistencyProof concurrent stamp — fetch_add + seqlock + stress tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
