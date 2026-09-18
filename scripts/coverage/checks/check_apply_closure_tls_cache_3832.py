#!/usr/bin/env python3
"""Issue #3832: apply_closure happy path skips process-wide closures_mtx_.

Wave2 still shared_lock(closures_mtx_) on every apply before copying
Closure. Multi-fiber high-rate apply serialized on one process mutex.
Fix: epoch-local TLS cache of last-N Closure copies, invalidated on
unique-write / erase (closures_apply_epoch_) and densify window_seq.
Tombstone + densify-stale hard-refuse retained.

Contract:
  AC1 Hot apply: TLS lookup before shared_lock; stamp + slots
  AC2 Tombstone / densify-stale refuse retained; epoch bump on unique_lock
  AC3 Microbench / contended multi-worker in existing test; build wiring;
      no invent test_issue_*.cpp; no docs/design/

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    test = _read("tests/compiler/test_apply_closure_envframe_soa.cpp")
    build = _read("build.py")

    must("kApplyClosureTlsCacheIssue = 3832", "AC1 stamp", flat)
    must("Issue #3832", "AC1 cite", flat)
    must("try_apply_closure_tls_lookup", "AC1 TLS lookup", flat)
    must("store_apply_closure_tls", "AC1 TLS store", flat)
    must("g_last_window_seq", "AC1 densify invalidate", flat)

    # Happy path: TLS before shared_lock on closures_mtx_
    pos_tls = flat.find("try_apply_closure_tls_lookup(*this, cid, cl_copy, tombstoned)")
    # Issue #3867: the apply-path mutex is sharded — the lock is
    # closures_shards_[closures_shard_index(cid)].mu (same ordering intent).
    pos_mtx = flat.find(
        "std::shared_lock<std::shared_mutex> rlock(closures_shards_[closures_shard_index(cid)].mu);", pos_tls
    )
    if pos_tls < 0 or pos_mtx < 0 or pos_mtx < pos_tls:
        fails.append("AC1: TLS lookup must precede the closures shard shared_lock on apply path")

    must("closures_apply_epoch_", "AC1 epoch field", ev)
    must("kApplyClosureTlsCacheIssue = 3832", "AC1 evaluator stamp", ev)
    must("bump_closures_apply_epoch", "AC2 epoch bump", ev)

    must("production_apply_closure_densify_hard_refuse", "AC2 densify refuse", flat)
    must("closure_apply_use_site_ok", "AC2 tombstone protocol", flat)
    must("bump_closures_apply_epoch(); // Issue #3832", "AC2 unique-write bump", flat)

    must("3832 AC1", "AC3 test AC1", test)
    must("3832 AC3", "AC3 microbench", test)
    must("3832 AC2", "AC3 tombstone", test)
    must("ac3832_tls_cache_happy_path", "AC3 runner", test)
    must("check_apply_closure_tls_cache_3832", "AC3 build.py", build)

    if "schema-3832" in flat or "schema-3832" in ev:
        fails.append("AC3: new schema-3832 query key")
    if (ROOT / "tests" / "compiler" / "test_issue_3832.cpp").is_file():
        fails.append("AC3: forbidden tests/compiler/test_issue_3832.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3832.cpp").is_file():
        fails.append("AC3: forbidden tests/issues/test_issue_3832.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3832-*")):
            fails.append(f"AC3: docs/design/{f.name} present (forbidden)")

    # Issue #3867: closures 16-shard conversion (extend #3832, no new check file).
    for needle, label in [
        ("Issue #3867", "AC3867 ev cite"),
        ("struct ClosuresShard", "AC3867 shard struct"),
        ("kClosuresShardCount = 16", "AC3867 shard count"),
    ]:
        if needle not in ev:
            fails.append(f"{label}: missing {needle!r}")
    gc3867 = _read("src/compiler/evaluator_gc.cpp")
    env3867 = _read("src/compiler/evaluator_env.cpp")
    if "cl_sh : closures_shards_)" not in gc3867:
        fails.append("AC3867: gc walks not sharded")
    if "cl_sh : closures_shards_)" not in env3867:
        fails.append("AC3867: env walks not sharded")
    t3867 = _read("tests/compiler/test_apply_closure_envframe_soa.cpp")
    if "Issue #3867" not in t3867:
        fails.append("AC3867 test cite: missing Issue #3867")
    if (ROOT / "tests" / "issues" / "test_issue_3867.cpp").is_file():
        fails.append("AC3867: test_issue_3867.cpp present (forbidden #81967)")

    if fails:
        print("FAIL #3832 apply_closure_tls_cache:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3832 apply_closure_tls_cache: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
