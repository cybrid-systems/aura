#!/usr/bin/env python3
"""Issue #3257: deferred_hybrid re-arm last-look before partial peel.

#3168 attributes [initial_size, current) when rearm_observed_mid_loop.
Residual: concurrent record_dependency can append after that snapshot
and before relower_define_blocks, so a partial peel omits the new edge.

#3257: last-look armed immediately before attribution; after attribution
if the tail grew, fail-closed to full (bounded once). Soft + armed==0:
acquire load only (zero extra lock). Distinguisher: existing
cascade_rearm_new_edge_only_total vs partial_forced_full_by_impact_total.

Contract:
  AC1  last-look armed + attr_seen_size; fail-closed when size grew
  AC2  #3168 attribution still prefers partial (new-edge-only counter)
  AC3  Soft + armed==0: acquire load; need_lock gate preserved
  AC4  concurrent soak in test_cascade_decision_residual_atomic
  AC5  linter wired after #3256; no docs/design; no test_issue_3257.cpp

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

    svc = _read("src/compiler/service.ixx")
    t = _read("tests/compiler/test_cascade_decision_residual_atomic.cpp")
    build = _read("build.py")
    obs = _read("src/compiler/observability_metrics.h")

    rel_pos = svc.find("std::size_t relower_dirty_defines_from_workspace()")
    # Window expanded from 16000 after #3381 grew the function body
    # (added caller-union block before the attribution loop — pushed
    # the #3257 last-look / fail-closed / attribution patterns past
    # the original 16000-char window). #3484 zero-mask fail-closed
    # pushed last-look past 24000.
    rel_win = svc[rel_pos : rel_pos + 28000] if rel_pos >= 0 else ""

    must("Issue #3257", "AC1 cite", rel_win)
    must("attr_seen_size", "AC1 snapshot", rel_win)
    must("post_attr_armed", "AC1 last-look", rel_win)
    must("size_now > attr_seen_size", "AC1 fail-closed", rel_win)
    must("ac3257_1_last_look_source", "AC1 test", t)

    must("Issue #3168: prefer new-edge-only mark over full fallback", "AC2 attribution", rel_win)
    must("cascade_rearm_new_edge_only_total", "AC2 distinguisher", obs)
    must("cascade_rearm_new_edge_only_total.fetch_add", "AC2 bump", rel_win)
    must("ac3257_2_attribution_prefers_partial", "AC2 test", t)

    must("const bool need_lock =", "AC3 need_lock", rel_win)
    last = rel_win.find("Issue #3257: last-look armed immediately before attribution")
    last_win = rel_win[last : last + 900] if last >= 0 else ""
    must("memory_order_acquire", "AC3 acquire", last_win)
    must("ac3257_3_soft_zero_extra", "AC3 test", t)

    must("ac3257_4_concurrent_rearm_soak", "AC4 soak", t)
    must("public_note_stale_dep_reject", "AC4 inject", t)

    must("ac3257_5_source_and_linter", "AC5 test", t)
    must("check_deferred_hybrid_rearm_last_look_3257", "AC5 build.py", build)
    prev = build.find("check_mailbox_defer_slo_hold_unify_3256")
    ours = build.find("check_deferred_hybrid_rearm_last_look_3257")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3256")

    if (ROOT / "tests" / "issues" / "test_issue_3257.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3257.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3257.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3257.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3257-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3257 deferred_hybrid_rearm_last_look:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3257 deferred_hybrid_rearm_last_look: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
