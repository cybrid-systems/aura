#!/usr/bin/env python3
"""Issue #3262: GC safepoint restamp-after-unlock + audit acquire-load.

probe_linear_ownership_at_gc_safepoint must drop closures_mtx_ /
env_frames_mtx_ shared_locks before auto_restamp_pinned_stable_refs_at
(restamp mutates StableNodeRef / cow pin table). Keep #1867 release
writes on record_linear_gc_probe; pair them with acquire-load of
linear_ownership_gc_violations_prevented_total in the audit (and the
dashboard query). Stale/resync stay relaxed. Soft empty-probe path
unchanged (one restamp after unlock).

Contract:
  AC1  restamp after dual shared_lock RAII scope
  AC2  audit + dashboard acquire-load viol; stale/resync relaxed
  AC3  #1867 release writes kept on record_linear_gc_probe
  AC4  probe restamps exactly once after unlock (zero extra)
  AC5  extend existing suites; linter after #3261; no invent

Exit 0 = all rows satisfied.

Follow-up #3263: quote_lambda marker sample-once + drop dead stripped bump.

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

    gc = _read("src/compiler/evaluator_gc.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    batch = _read("tests/compiler/test_linear_batch.cpp")
    steal = _read("tests/compiler/test_linear_provenance_steal_gc_closed_loop.cpp")
    build = _read("build.py")
    l3261 = _read("scripts/coverage/checks/check_flush_mutation_boundary_toctou_3261.py")

    pos = gc.find("void Evaluator::probe_linear_ownership_at_gc_safepoint()")
    end = gc.find("void Evaluator::resync_linear_jit_gc_roots_after_invalidate", pos)
    win = gc[pos:end] if pos >= 0 and end > pos else ""
    must("Issue #3262", "AC1 cite", win)
    must("std::shared_lock<std::shared_mutex> cl_lock(closures_mtx_)", "AC1 cl lock", win)
    must("std::shared_lock<std::shared_mutex> env_lock(env_frames_mtx_)", "AC1 env lock", win)
    rec = win.find("record_linear_gc_probe")
    rest = win.find("auto_restamp_pinned_stable_refs_at")
    if rec < 0 or rest < 0 or rest < rec:
        fails.append("AC1: restamp must follow record_linear_gc_probe")
    else:
        mid = win[rec:rest]
        if "}" not in mid:
            fails.append("AC1: restamp still inside lock RAII scope")
        if "std::shared_lock" in mid:
            fails.append("AC1: lock still held across restamp")
    must("AFTER dual shared_locks", "AC1 after-unlock comment", win)
    must("ac3262_1_restamp_after_locks", "AC1 test", steal)
    must("run_3262_source", "AC1 batch", batch)

    apos = gc.find("bool Evaluator::run_linear_gc_root_audit")
    awin = gc[apos : apos + 2200] if apos >= 0 else ""
    must(
        "linear_ownership_gc_violations_prevented_total.load(std::memory_order_acquire)",
        "AC2 audit acquire",
        awin,
    )
    must(
        "linear_ownership_gc_root_stale_hits_total.load(std::memory_order_relaxed)",
        "AC2 stale relaxed",
        awin,
    )
    must(
        "linear_ownership_gc_env_version_resync_total.load(std::memory_order_relaxed)",
        "AC2 resync relaxed",
        awin,
    )
    dpos = obs.find("linear_ownership_gc_violations_prevented_total.load")
    dwin = obs[dpos : dpos + 120] if dpos >= 0 else ""
    must("memory_order_acquire", "AC2 dashboard acquire", dwin)
    must("ac3262_2_audit_acquire", "AC2 test", steal)

    rpos = gc.find("static void record_linear_gc_probe")
    rwin = gc[rpos : rpos + 1800] if rpos >= 0 else ""
    must("Issue #1867", "AC3 keep 1867 cite", rwin)
    must("memory_order_release", "AC3 keep release writes", rwin)
    if rwin.count("memory_order_release") < 6:
        fails.append("AC3: record_linear_gc_probe dropped release writes")
    must("ac3262_3_keep_1867_release", "AC3 test", steal)

    if win.count("auto_restamp_pinned_stable_refs_at") != 1:
        fails.append("AC4: probe must restamp exactly once after unlock")
    must("ac3262_4_quiet_probe_zero_extra", "AC4 test", steal)

    must("ac3262_5_source_and_linter", "AC5 test", steal)
    must("check_gc_safepoint_restamp_lock_3262", "AC5 build.py", build)
    must("Issue #3262", "AC5 evaluator.ixx", ixx)
    prev = build.find("check_flush_mutation_boundary_toctou_3261")
    ours = build.find("check_gc_safepoint_restamp_lock_3262")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3261")
    must("3262", "AC5 extend 3261 linter", l3261)
    if (ROOT / "tests" / "issues" / "test_issue_3262.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3262.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3262.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3262.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3262-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    q = _read("src/compiler/evaluator_primitives_query_tail.cpp")
    if "schema-3262" in q or "schema-3262" in steal or "schema-3262" in batch:
        fails.append("AC5: new schema-3262 query key (SlimSurface)")

    if fails:
        print("FAIL #3262 gc_safepoint_restamp_lock:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3262 gc_safepoint_restamp_lock: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
