#!/usr/bin/env python3
"""Issue #3135: P0 soundness — relower_dirty_defines drain + impact_ub +
partial peel not atomic vs concurrent record_dependency re-arm
(residual of #3067/#3097).

Contract:
  AC1 service.ixx — cascade_decision_mtx_ (std::mutex, ordered after
     dep_graph_mtx_ + mutate_mtx_) + record_dependency acquires it
     around the deferred_hybrid_edges_.emplace_back + store(1) reject
     pair; relower_dirty_defines_from_workspace acquires it for the
     drain + impact_ub consult + partial/full decision window + re-
     checks deferred_hybrid_armed_ immediately before peel.
  AC2 Soft / sandbox=off + single-fiber + clean (armed==0) skips the
     lock for zero cost. Lock is acquired conditionally via
     std::defer_lock + .lock() pattern on need_lock gate.
  AC3 Quiet happy path (no concurrent reject) — single existing hard-
     AND consult path, no extra lock when need_lock is false.
  AC4 Existing #3067 (drain at entry) + #3097 (impact_ub consult +
     partial_forced_full_by_impact_total counter) preserved. Re-check
     on re-arm forces full + mark_all_blocks_dirty.
  AC5 Regression test in tests/compiler/ (src/-aligned per #81967).
     No tests/issues/test_issue_3135.cpp. No docs/design/3135-*
     (#1655).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ixx = _read("src/compiler/service.ixx")
    test = _read("tests/compiler/test_cascade_decision_residual_atomic.cpp")

    # ── AC1: cascade_decision_mtx_ + record_dependency reject lock ──
    field_pos = ixx.find("cascade_decision_mtx_")
    if field_pos == -1:
        fails.append("AC1: cascade_decision_mtx_ not declared")
    else:
        # Field declaration block: anchor backwards to include comment.
        field_start = max(0, field_pos - 1500)
        field_end = field_pos + 1500
        field_block = ixx[field_start:field_end]
        must("Issue #3135", "AC1 field block cites #3135", field_block)
        must("std::mutex", "AC1 lock is std::mutex", field_block)
        must("cascade_decision_mtx_", "AC1 lock named cascade_decision_mtx_", field_block)
        must("cascade_decision_mtx_", "AC1 lock named in comment", field_block)

    # record_dependency: cascade_guard inside reject branch.
    rd_pos = ixx.find("void record_dependency(const std::string& caller, const std::string& callee)")
    if rd_pos == -1:
        rd_pos = ixx.find("void record_dependency(")
    if rd_pos == -1:
        fails.append("AC1: record_dependency definition not found")
    else:
        rd_end = rd_pos + 6000
        rd_block = ixx[rd_pos:rd_end]
        must("cascade_guard(cascade_decision_mtx_)", "AC1 record_dependency acquires cascade_decision_mtx_", rd_block)
        must("Issue #3135", "AC1 record_dependency reject path cites #3135", rd_block)
        guard_pos = rd_block.find("cascade_guard(cascade_decision_mtx_)")
        reject_pos = rd_block.find("epoch_before != epoch_after")
        if guard_pos == -1 or reject_pos == -1:
            fails.append(f"AC1: guard / reject anchor missing (guard={guard_pos}, reject={reject_pos})")
        elif not (reject_pos < guard_pos):
            fails.append(
                f"AC1: cascade_guard must sit INSIDE the reject branch (got reject={reject_pos}, guard={guard_pos})"
            )

    # relower_dirty_defines_from_workspace: critical section + re-check.
    rel_pos = ixx.find("std::size_t relower_dirty_defines_from_workspace()")
    if rel_pos == -1:
        fails.append("AC1: relower_dirty_defines_from_workspace not found")
    else:
        # Window expanded after #3381/#3484 grew the peel body (caller
        # union + zero-mask fail-closed before the impact_ub consult).
        rel_end = rel_pos + 20000
        rel_block = ixx[rel_pos:rel_end]
        must("Issue #3135", "AC1 relower cites #3135", rel_block)
        must("cascade_decision_mtx_", "AC1 relower uses cascade_decision_mtx_", rel_block)
        must("initial_armed", "AC1 relower snapshots initial_armed", rel_block)
        must("need_lock", "AC1 relower gates need_lock", rel_block)
        must("production_defaults_active", "AC1 relower gates on production_defaults_active", rel_block)
        must("re_armed_now", "AC1 relower re-checks re_armed_now before peel", rel_block)
        must("mark_all_blocks_dirty", "AC1 relower force-fulls via mark_all_blocks_dirty", rel_block)

    # ── AC2: Soft skip — defer_lock + conditional .lock() ──
    if rel_pos != -1:
        # clang-format may split `need_lock = initial_armed || ...`
        # across multiple lines, so check for the tokens in proximity
        # rather than the literal joined string.
        if "need_lock" not in rel_block or "initial_armed ||" not in rel_block:
            fails.append("AC2: need_lock is the Soft-skip gate (need_lock + initial_armed || tokens missing)")
        must("cascade_guard.lock()", "AC2 lock acquired conditionally via .lock()", rel_block)
        must("defer_lock", "AC2 defer_lock pattern (no acquisition until gate passes)", rel_block)

    # ── AC3: guard inside reject branch (no lock on idem path) ──
    if rd_pos != -1:
        guard_pos = rd_block.find("cascade_guard(cascade_decision_mtx_)")
        reject_pos = rd_block.find("epoch_before != epoch_after")
        return_pos = rd_block.find("return;", guard_pos) if guard_pos != -1 else -1
        if guard_pos == -1:
            fails.append("AC3: guard missing in record_dependency")
        elif reject_pos == -1:
            fails.append("AC3: reject branch anchor missing in record_dependency")
        elif not (reject_pos < guard_pos):
            fails.append(f"AC3: guard must sit inside reject branch (reject={reject_pos}, guard={guard_pos})")
        if return_pos != -1 and guard_pos != -1 and not (guard_pos < return_pos):
            fails.append(
                f"AC3: guard must span emplace + store (before return) (guard={guard_pos}, return={return_pos})"
            )

    # ── AC4: #3067 + #3097 paths preserved + re-check force-full ──
    if rel_pos != -1:
        must("drain_deferred_hybrid_cascade_()", "AC4 #3067 drain preserved", rel_block)
        must("impact_upper_bound_for_entry_", "AC4 #3097 impact_ub consult preserved", rel_block)
        # Issue #3310: partial-gate may now route through
        # should_partial_relower_impact_checked_prod (which delegates to
        # should_partial_relower_impact_checked). Accept either form —
        # the AC4 contract is "partial-gate check preserved", and the
        # delegating helper satisfies that contract.
        if (
            "should_partial_relower_impact_checked" not in rel_block
            and "should_partial_relower_impact_checked_prod" not in rel_block
        ):
            fails.append(
                "AC4 #3097 partial-gate check preserved: missing 'should_partial_relower_impact_checked[_prod]'"
            )
        must("partial_forced_full_by_impact_total", "AC4 existing counter reused (no new metric key)", rel_block)
        # Re-check force-full path uses the existing counter.
        must(
            "metrics_.partial_forced_full_by_impact_total.fetch_add", "AC4 force-full bumps existing counter", rel_block
        )

    # ── AC5: src-aligned test, no test_issue_3135.cpp, no plan doc ──
    must("Issue #3135", "AC5 regression test cites", test)
    must("cascade_decision_mtx_", "AC5 regression test asserts the lock", test)
    if (ROOT / "tests" / "issues" / "test_issue_3135.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3135.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "compiler" / "test_issue_3135.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3135.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3135-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    must(
        "check_cascade_decision_residual_atomic_3135",
        "AC6 build.py wiring",
        _read("build.py") + _read("pyproject.toml"),
    )

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3135 cascade-decision residual atomic — all 5 AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
