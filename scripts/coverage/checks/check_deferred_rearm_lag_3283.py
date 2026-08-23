#!/usr/bin/env python3
"""Issue #3283 linter — close deferred-hybrid re-arm lag before partial peel.

Residual (Compiler Pipeline + Incremental residual-gap review): under
production multi-fiber mutate, record_dependency may reject a stale edge and
enqueue into deferred_hybrid_edges_. relower_dirty_defines_from_workspace
drains deferred at entry and holds cascade_decision_mtx_ for the
drain → snapshot → impact_ub → partial/full decision window. Residual: a
concurrent record_dependency / record_block_dependency can re-arm additional
deferred edges AFTER the size/snapshot used for impact consult but BEFORE (or
during) the partial peel — those late edges are not in the dirty mask /
impact_ub used by should_partial_relower_impact_checked, so a partial
decision can under-mark callers that should have been cascaded.

Fix (minimal, fail-closed):
- deferred_hybrid_gen_ generation counter bumped under cascade_decision_mtx_
  at BOTH emplace sites (record_dependency + record_block_dependency —
  #3135 parity: record_block_dependency previously appended WITHOUT the
  lock, the actual re-arm hole).
- relower snapshots gen0 after the entry drain + lock and re-checks it
  immediately before committing to partial: any bump during the decision
  window = a concurrent stale-reject re-armed edges the drain/snapshot
  never saw → force full (AC1(b) take-full, distinguisher
  partial_forced_full_by_impact_total).

Gate rows:
  G1  service.ixx cites Issue #3283 and has the deferred_hybrid_gen_ member.
  G2  record_dependency stale-reject bumps the gen under cascade_decision_mtx_.
  G3  record_block_dependency stale-reject takes cascade_decision_mtx_ and
      bumps the gen (#3135 parity — closes the unlocked re-arm path).
  G4  relower snapshots gen0 under the lock (after initial_deferred_edges_size).
  G5  relower re-checks gen immediately before partial peel and force-fulls
      (mark_all_blocks_dirty + partial_forced_full_by_impact_total) on gen
      move + cone hit.
  G6  test ACs in tests/compiler/test_cascade_decision_residual_atomic.cpp
      (#3135 suite home, #81967) exercising concurrent re-arm → no clean
      partial on the lagging caller.
  G7  build.py wires this linter.
  G8  no docs/design/3283-* (per #1655), no tests/issue*/test_issue_3283.cpp
      (per #81967).

Exit 0 = all rows satisfied.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

failures: list[str] = []


def must(ok: bool, label: str) -> None:
    if ok:
        print(f"  OK: {label}")
    else:
        failures.append(label)
        print(f"  FAIL: {label}")


def read(rel: str) -> str:
    p = ROOT / rel
    try:
        return p.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def main() -> int:
    print("=== #3283 deferred-hybrid re-arm lag linter ===")
    svc = read("src/compiler/service.ixx")
    test = read("tests/compiler/test_cascade_decision_residual_atomic.cpp")
    build = read("build.py")

    must("Issue #3283" in svc, "G1: service.ixx cites Issue #3283")
    must("deferred_hybrid_gen_" in svc, "G1: deferred_hybrid_gen_ member present")

    # G2: record_dependency stale-reject bumps gen under cascade_decision_mtx_
    rd = svc[svc.find("void record_dependency(") :]
    rd_block = rd[: rd.find("void record_block_dependency(")]
    must(
        "cascade_decision_mtx_" in rd_block and "deferred_hybrid_gen_" in rd_block,
        "G2: record_dependency bump under cascade_decision_mtx_",
    )

    # G3: record_block_dependency stale-reject takes the lock + bumps gen
    rbd = rd[rd.find("void record_block_dependency(") :]
    rbd_stale = (
        rbd[: rbd.find("void mirror_block_dep_edge_unlocked_")]
        if "void mirror_block_dep_edge_unlocked_" in rbd
        else rbd[:4000]
    )
    must(
        "cascade_decision_mtx_" in rbd_stale and "deferred_hybrid_gen_" in rbd_stale,
        "G3: record_block_dependency lock + gen bump (#3135 parity)",
    )

    # G4: gen0 snapshot after the size snapshot in relower
    rel = svc[svc.find("std::size_t relower_dirty_defines_from_workspace()") :]
    rel = rel[: rel.find("void clear_define_cache_v2")]
    must("gen0 = deferred_hybrid_gen_.load" in rel, "G4: gen0 snapshot under lock")

    # G5: pre-peel re-check + force full
    must("deferred_hybrid_gen_.load(std::memory_order_acquire) != gen0" in rel, "G5a: pre-peel gen re-check present")
    must(
        "cone_hit" in rel and "mark_all_blocks_dirty" in rel and "partial_forced_full_by_impact_total" in rel,
        "G5b: gen-move + cone hit → force full (AC1(b))",
    )

    # G6: test ACs
    must("ac3283" in test or "3283" in test, "G6: test cites Issue #3283")
    must("concurrent" in test or "rearm" in test or "re-arm" in test, "G6: concurrent re-arm exercise present")

    # G7: build.py wiring
    must("check_deferred_rearm_lag_3283.py" in build, "G7: build.py wires linter")

    # G8: no docs/design / no tests/issue
    docs_ok = True
    if (ROOT / "docs/design").exists():
        docs_ok = not any(p.name.startswith("3283-") for p in (ROOT / "docs/design").glob("3283-*"))
    must(docs_ok, "G8a: no docs/design/3283-* per #1655")
    must(
        not (ROOT / "tests" / "issues" / "test_issue_3283.cpp").exists(),
        "G8b: no tests/issues/test_issue_3283.cpp per #81967",
    )

    print()
    if failures:
        print(f"#3283 linter FAILED: {len(failures)} gate(s) — {failures}")
        return 1
    print("#3283 linter: all gates OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
